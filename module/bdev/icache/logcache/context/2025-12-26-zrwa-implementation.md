# ZRWA Implementation Context - 2025-12-26

## 목표
ZNS SSD의 ZRWA (Zone Random Write Area) 기능을 활용하여 캐시 레이어 구현

## 핵심 개념

### ZRWA 파라미터 (디바이스)
- **ZRWAFG (Flush Granularity)**: 4 blocks = 16KB
- **ZRWASZ (Size)**: 256 blocks = 1MB window
- **ZRWACAP**: 1 (Explicit flush 지원)

### Write Pointer 관리
- **write_pointer (SW WP)**: 소프트웨어가 추적하는 WP, write 완료 시 증가
- **flushed_wp (Device WP)**: 실제 디바이스 WP, ZRWA flush 완료 시 증가

## 완료된 작업

### 1. can_submit() 수정
```cpp
bool can_submit(uint64_t offset, size_t len) const {
    if (!zone_opened) return false;
    if (flush_in_progress) return false;
    // ZNS sequential write constraint
    if (offset < write_pointer) return false;
    if (is_aligned(offset, len)) {
        // Aligned: within ZRWA window from flushed_wp
        return offset + len <= flushed_wp + ZONE_MAX_LBA_DISTANCE;
    } else {
        // Non-aligned: must be at write_pointer
        return offset == write_pointer;
    }
}
```

### 2. ZRWA Flush 명령어 수정
- CDW10-11: end LBA (zone SLBA가 아님!)
- CDW13: 0x11 (Flush Explicit)
- ZRWAFG alignment 적용 (4 blocks 단위로 정렬)

```cpp
int flush_zrwa_async(uint64_t zone_id) {
    // ZRWAFG alignment
    static constexpr uint64_t ZRWAFG_BLOCKS = 4;
    uint64_t aligned_count = (flush_count / ZRWAFG_BLOCKS) * ZRWAFG_BLOCKS;
    if (aligned_count == 0) {
        zq.flush_in_progress = false;
        return 0;
    }
    uint64_t end_lba = device_wp_blocks + aligned_count - 1;

    // NVMe command
    struct spdk_nvme_cmd cmd = {};
    cmd.opc = SPDK_NVME_OPC_ZONE_MGMT_SEND;
    *(uint64_t *)&cmd.cdw10 = end_lba;  // End LBA!
    cmd.cdw13 = 0x11;  // Flush Explicit
    // ...
}
```

### 3. maybe_flush_zrwa() 활성화
이전에 비활성화되어 있던 함수를 다시 활성화

### 4. Flush 완료 콜백 수정
```cpp
static void zrwa_flush_completion(...) {
    zq.flush_in_progress = false;
    if (success) {
        zq.flushed_wp = flush_wp;
    }
    // Process pending writes first
    device->process_zone_pending(zone_id);
    // Then check if more flush needed
    if (zq.needs_flush()) {
        device->flush_zrwa_async(zone_id);
    }
}
```

## 해결된 문제

### Pending Queue 순서 문제
로그에서 발견된 문제:
```
can't submit entry offset=1445888, len=16384 (zone_opened=1)
...
writev_cache_async: offset=1474560 submitting directly
```

**원인**: pending queue에 요청이 있는데 새 요청이 바로 submit됨

**결과**: 1474560이 먼저 완료되면 WP가 증가하고, 1445888은 `offset < write_pointer` 조건에 걸려 영원히 submit 안됨

### 해결 (적용됨)
`write_cache_async()`와 `writev_cache_async()`에서 pending queue가 비어있지 않으면 무조건 queue에 추가:

```cpp
// pending이 있으면 순서 보장을 위해 queue에 넣음
if (!zq.pending.empty() || !zq.can_submit(offset, total_len)) {
    zq.pending.push(entry);
    return 0;
}
```

### 연속 완료 WP 관리 (이미 구현됨)
`complete_io()` -> `update_write_pointer()`에서:
- inflight_ios의 첫 번째(가장 낮은 offset) IO가 WP와 같고 completed면 WP 증가
- 연속 완료된 범위까지 반복

```cpp
bool update_write_pointer() {
    while (!inflight_ios.empty()) {
        auto it = inflight_ios.begin();
        if (it->first == write_pointer && it->second.completed) {
            write_pointer = it->second.end_offset;
            inflight_ios.erase(it);
        } else {
            break;
        }
    }
    return write_pointer > old_wp;
}
```

## 파일 위치
- `/home/sejun000/spdk_25/module/bdev/icache/logcache/log_cache_wrapper.cpp`

## NVMe 명령어 참고

### Zone Open with ZRWA
```bash
nvme io-passthru /dev/nvme1n2 --opcode=0x79 --cdw10=0 --cdw11=0 --cdw13=0x203
# 0x203 = Open (0x03) | ZRWA allocation (1 << 9)
```

### ZRWA Flush
```bash
nvme io-passthru /dev/nvme1n2 --opcode=0x79 --cdw10=3 --cdw11=0 --cdw13=0x11
# cdw10 = end_lba (e.g., 3 means flush LBA 0-3)
# 0x11 = Flush Explicit
```

## 추가 수정: Zone Open 시 WP 초기화

**문제**: zone 1 이후부터 WP=0으로 시작해서 can_submit 체크 실패
- zone 1의 offset은 2147483648 (2GB)
- WP=0, flushed_wp=0 상태에서 `offset + len <= flushed_wp + 1MB` 체크 실패
- pending만 계속 쌓이고 submit 안됨 -> 데드락

**해결**: zone open 완료 시 WP와 flushed_wp를 zone 시작 offset으로 초기화

```cpp
// zone_open_zrwa_completion에서
if (success) {
    zq.zone_opened = true;
    uint64_t zone_start_offset = zone_id * device->m_cache_zone_blocks * device->m_block_size;
    zq.write_pointer = zone_start_offset;
    zq.flushed_wp = zone_start_offset;
}
```

## Zone Capacity 경계 Split Write (2025-12-26 추가)

### 문제 발견
ZONE INVALID WRITE (01/bc) 에러 발생:
- Zone 0 capacity: 0x43500 blocks = 275712 blocks
- Write at lba 275710 with len 4 → blocks 275710-275713
- Blocks 275712-275713이 ZCAP 초과!

### 원인 분석
1. **Timeout flush 문제**: write_buffer가 4 blocks 미만일 때도 flush
   - 2 blocks (8KB) 단위로 flush되면 16KB alignment가 깨짐
   - Zone 시작이 block 0이 아닌 block 2에서 시작하면
   - 마지막 16KB write가 zone capacity를 초과

2. **Zone 경계 처리 부재**: flush_write_buffer가 zone capacity 체크 없이 연속 write

### 해결: Split Write 구현

```cpp
// flush_write_buffer에서 zone capacity 경계 체크
void LogCacheAsync::flush_write_buffer() {
    // 1. cache_offsets 먼저 모두 획득
    for (각 block) {
        cache_->append_block_metadata(..., &cache_offset);
        cache_offsets.push_back(cache_offset);
    }

    // 2. zone capacity 내에서 연속된 offset 그룹화
    while (i < total_blocks) {
        uint64_t zone_end = zone_start + zone_capacity;

        while (i + count < total_blocks) {
            // 연속성 체크
            if (actual_next != expected_next) break;
            // zone capacity 체크
            if (actual_next + block_size > zone_end) break;
            count++;
        }
        groups.push_back({start_idx, count, first_offset});
    }

    // 3. 각 그룹별 별도 write 발행
    for (각 group) {
        device_->writev_cache_async(group.first_offset, iovs, count,
                                    group_len, zone_write_done, zctx);
    }
}
```

### 동작 예시
```
4 blocks 쓰기 (blocks 275710-275713):
- LogCache 할당:
  - Block 275710: zone 0, offset 1129308160
  - Block 275711: zone 0, offset 1129312256
  - Block 275712: zone 1 (segment transition), offset 2147483648
  - Block 275713: zone 1, offset 2147487744

- 그룹화:
  - Group 1: zone 0, 2 blocks, 8KB
  - Group 2: zone 1, 2 blocks, 8KB

- 별도 write 발행:
  - writev(zone 0, 8KB)
  - writev(zone 1, 8KB)
```

### 추가 구조체
```cpp
struct WriteBufferFlushCtx {
    std::vector<BufferedBlock> blocks;
    std::vector<uint64_t> cache_offsets;
    std::set<CacheIo*> pending_ios;
    std::atomic<int> outstanding_writes{0};  // Split write 추적
    std::atomic<int> first_error{0};
};

struct ZoneWriteCtx {
    WriteBufferFlushCtx *parent;
    struct iovec *iovs;
    int iovcnt;
};
```

## ublk 설정 (2025-12-26 추가)

### 목적
NVMe-TCP 오버헤드를 줄이기 위해 ublk (userspace block device)로 전환

### NVMe-TCP vs ublk 비교
```
NVMe-TCP 방식:
  fio → kernel NVMe driver → TCP → SPDK NVMf target → icache bdev

ublk 방식:
  fio → kernel ublk driver → io_uring → SPDK ublk target → icache bdev
```

ublk는 TCP 스택 없이 io_uring을 통해 직접 SPDK와 통신하여 오버헤드 감소

### 설치 과정

#### 1. 커널 ublk 모듈 빌드
```bash
cd /home/sejun000/linux
# CONFIG_BLK_DEV_UBLK=m 설정
make menuconfig  # Device Drivers → Block devices → Userspace block device driver
make -j$(nproc)
sudo make modules_install
sudo modprobe ublk_drv
```

#### 2. liburing 업그레이드 (2.2+ 필요)
시스템 liburing 2.1은 `io_uring_register_buf_ring` 등 함수 미지원

```bash
cd /home/sejun000
git clone https://github.com/axboe/liburing.git
cd liburing
./configure --prefix=/usr
make -j$(nproc)
sudo make install

# 구버전 충돌 제거
sudo rm /usr/lib/x86_64-linux-gnu/liburing*
sudo ldconfig
```

#### 3. SPDK ublk 빌드
```bash
cd /home/sejun000/spdk_25

# 커널 헤더 설치 (ublk_cmd.h 필요)
cd /home/sejun000/linux
sudo make headers_install INSTALL_HDR_PATH=/usr

# SPDK 빌드
cd /home/sejun000/spdk_25
# CONFIG_UBLK=y (mk/config.mk에서 설정됨)
./configure --with-uring
make clean && make -j$(nproc)
```

### 스크립트 수정

#### create_tier.sh 변경
```bash
# 기본값 변경
UBLK_ENABLE=${UBLK_ENABLE:-1}      # ublk 활성화 (기본)
UBLK_DEV_ID=${UBLK_DEV_ID:-0}      # /dev/ublkb0
NVMF_ENABLE=${NVMF_ENABLE:-0}      # NVMe-TCP 비활성화 (기본)

# ublk 시작 추가
if [[ "${UBLK_ENABLE}" != "0" ]]; then
    rpc_call "start ublk for ${ICACHE_NAME}" \
        ublk_start_disk "${ICACHE_NAME}" "${UBLK_DEV_ID}"
fi
```

#### exit_tgt.sh 변경
```bash
# ublk 먼저 중지
if [[ -e "/dev/ublkb${UBLK_DEV_ID}" ]]; then
    "${RPC[@]}" ublk_stop_disk "${ICACHE_NAME}" 2>/dev/null || true
    sleep 1
fi
```

#### fio_icache.sh 변경
```bash
# ublk 우선 확인
UBLK_DEVICE="/dev/ublkb${UBLK_DEV_ID}"
if [ -e "$UBLK_DEVICE" ]; then
    DEVICE="$UBLK_DEVICE"
else
    # Fallback: NVMe-oF
    DEVICE=$(sudo nvme list | grep -i "ICACHE" | awk '{print $1}')
fi
```

### 실행 순서
```bash
# 1. ublk 커널 모듈 로드
sudo modprobe ublk_drv

# 2. SPDK 시작
./spdk_tgt.sh

# 3. icache + ublk 설정
./create_tier.sh

# 4. 테스트
./fio_icache.sh
```

### 에러 해결

| 에러 | 원인 | 해결 |
|------|------|------|
| `Method not found` (ublk_start_disk) | CONFIG_UBLK=n | mk/config.mk에서 CONFIG_UBLK=y |
| `liburing.h: No such file` | liburing-dev 미설치 | `sudo apt install liburing-dev` 또는 소스 빌드 |
| `linux/ublk_cmd.h: No such file` | 커널 헤더 미설치 | `sudo make headers_install INSTALL_HDR_PATH=/usr` |
| `IORING_OP_URING_CMD undeclared` | liburing 버전 낮음 (2.1) | liburing 2.2+ 소스 빌드 |
| `io_uring_register_buf_ring undefined` | 구버전 liburing 링크 | `/usr/lib/x86_64-linux-gnu/liburing*` 삭제 |
| `No such device` (-19) | ublk 커널 모듈 미로드 | `sudo modprobe ublk_drv` |

### 파일 위치
- liburing: `/home/sejun000/liburing`
- 커널 소스: `/home/sejun000/linux`
- SPDK config: `/home/sejun000/spdk_25/mk/config.mk`

## 다음 작업
1. `sudo modprobe ublk_drv` 후 create_tier.sh 재실행
2. ublk 기반 성능 테스트
