# Worker Thread Implementation Context - 2025-12-26

## 목표
ublk IO 수신(core 0)과 log_cache 처리(core 1)를 분리하여 성능 향상

## 아키텍처

```
fio -> ublk (core 0) -> spdk_thread_send_msg -> log_worker (core 1) -> log_cache
                                                      |
                                                      v
                                            completion callback
                                                      |
                                                      v
                          spdk_thread_send_msg -> orig_thread (core 0) -> spdk_bdev_io_complete
```

## 구현 내용

### 1. Worker Thread 생성 (vbdev_icache.c)

```c
#define ICACHE_WORKER_CORE  1  /* Dedicated core for log_wrapper */

static struct spdk_thread *g_log_worker_thread = NULL;
static struct spdk_io_channel *g_worker_cache_ch = NULL;
static struct spdk_io_channel *g_worker_backend_ch = NULL;
static bool g_worker_initialized = false;

struct worker_init_ctx {
    struct spdk_bdev_desc *cache_desc;
    struct spdk_bdev_desc *backend_desc;
    struct log_cache_ctx *log_ctx;
    int result;
    struct spdk_thread *caller_thread;
};

static int
icache_create_worker_thread(struct spdk_bdev_desc *cache_desc,
                            struct spdk_bdev_desc *backend_desc,
                            struct log_cache_ctx *log_ctx)
{
    struct spdk_cpuset cpuset;
    spdk_cpuset_zero(&cpuset);
    spdk_cpuset_set_cpu(&cpuset, ICACHE_WORKER_CORE, true);

    g_log_worker_thread = spdk_thread_create("log_worker", &cpuset);
    // ... io_channel 생성 및 poller 이동
}
```

### 2. IO Forwarding (vbdev_icache.c)

```c
struct icache_worker_msg {
    struct vbdev_icache *icache;
    struct spdk_bdev_io *bdev_io;
    struct spdk_thread *orig_thread;
};

static int
icache_forward_to_worker(struct vbdev_icache *icache, struct spdk_bdev_io *bdev_io)
{
    struct icache_worker_msg *msg = calloc(1, sizeof(*msg));
    msg->icache = icache;
    msg->bdev_io = bdev_io;
    msg->orig_thread = spdk_get_thread();

    spdk_thread_send_msg(g_log_worker_thread, icache_worker_process_write, msg);
    return 0;
}

static void
icache_worker_process_write(void *arg)
{
    struct icache_worker_msg *msg = arg;
    // ... log_cache_ctx_write_async 호출
}
```

### 3. Completion Path (vbdev_icache.c)

```c
struct icache_completion_msg {
    struct spdk_bdev_io *bdev_io;
    int status;
};

/* Worker thread에서 호출 */
static void
icache_worker_io_done(void *cb_arg, int status)
{
    struct icache_completion_msg *msg = calloc(1, sizeof(*msg));
    msg->bdev_io = bdev_io;
    msg->status = status;

    spdk_thread_send_msg(io_ctx->orig_thread, icache_complete_on_orig_thread, msg);
}

/* Original thread에서 호출 */
static void
icache_complete_on_orig_thread(void *arg)
{
    struct icache_completion_msg *msg = arg;
    spdk_bdev_io_complete(msg->bdev_io,
        msg->status ? SPDK_BDEV_IO_STATUS_FAILED : SPDK_BDEV_IO_STATUS_SUCCESS);
    free(msg);
}
```

### 4. Timeout Poller 이동 (log_cache_wrapper.cpp)

**문제**: write_buffer_timeout_poller가 RPC thread (core 0)에 등록되어 있어서 worker thread의 write buffer를 flush 못함

**해결**: worker thread 초기화 시 poller를 worker thread로 이동

```cpp
// log_cache_wrapper.cpp
extern "C" void
log_cache_ctx_move_poller_to_current_thread(struct log_cache_ctx *ctx)
{
    if (ctx->write_buffer_poller) {
        spdk_poller_unregister(&ctx->write_buffer_poller);
    }
    ctx->write_buffer_poller = spdk_poller_register(write_buffer_timeout_poller, ctx,
                                LogCacheAsync::WRITE_BUFFER_TIMEOUT_US);
    SPDK_NOTICELOG("Moved write_buffer poller to thread on core %d\n",
                   spdk_env_get_current_core());
}
```

```c
// vbdev_icache.c - icache_worker_init_on_thread()에서 호출
if (ctx->log_ctx) {
    log_cache_ctx_move_poller_to_current_thread(ctx->log_ctx);
}
```

## 스크립트 수정

### spdk_tgt.sh
```bash
# 두 코어 모두 사용 (ublk: core 0, log_worker: core 1)
CPU_MASK=${SPDK_TGT_CPUMASK:-0x3}
```

### create_tier.sh
```bash
# ublk는 core 0에서만 실행
UBLK_CPUMASK=${UBLK_CPUMASK:-0x1}
rpc_call "create ublk target on core 0" ublk_create_target -m "${UBLK_CPUMASK}"
```

### exit_tgt.sh
```bash
# 정리 순서: ublk 중지 -> icache bdev 삭제 -> spdk_tgt 종료
"${RPC[@]}" ublk_stop_disk "${ICACHE_NAME}" 2>/dev/null || true
"${RPC[@]}" bdev_icache_delete "${ICACHE_NAME}" 2>/dev/null || true
# ... kill spdk_tgt
```

## 발견된 문제들

### 1. ublk cpumask "Invalid argument" 에러
- 원인: spdk_tgt가 core 1만 사용 (0x2)하는데 ublk가 core 0 (0x1) 요청
- 해결: spdk_tgt를 0x3 (cores 0,1)으로 실행

### 2. IO가 submit만 되고 complete 안 됨
- 원인: timeout poller가 RPC thread에 있어서 worker thread의 write buffer flush 안 함
- 해결: log_cache_ctx_move_poller_to_current_thread() 추가

### 3. exit_tgt.sh 후 재시작 시 bdev 중복 에러
- 원인: spdk_tgt kill 전에 bdev 정리 안 됨
- 해결: bdev_icache_delete 호출 추가

## 파일 위치
- `/home/sejun000/spdk_25/module/bdev/icache/vbdev_icache.c`
- `/home/sejun000/spdk_25/module/bdev/icache/logcache/log_cache_wrapper.cpp`
- `/home/sejun000/spdk_25/module/bdev/icache/logcache/log_cache_wrapper.h`
- `/home/sejun000/spdk_25/ssd_waf/spdk_tgt.sh`
- `/home/sejun000/spdk_25/ssd_waf/create_tier.sh`
- `/home/sejun000/spdk_25/ssd_waf/exit_tgt.sh`

## 다음 작업
1. 빌드 후 "Moved write_buffer poller" 로그 확인
2. fio 테스트로 IO 완료 확인
3. 디버그 로그 제거 후 성능 테스트

## 디버그 로그 (제거 필요)
현재 vbdev_icache.c에 다음 로그들이 있음:
- "Worker processing write: offset=%lu len=%lu"
- "Worker write submit returned rc=%d"
- "Worker IO done: status=%d, sending to orig thread"

성능 테스트 전에 SPDK_NOTICELOG 제거 필요
