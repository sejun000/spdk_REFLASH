# Graph Specification

## Config 목록

| Config Name | CSV 파일 | stat.log 파일 | replay_trace 파일 |
|-------------|----------|---------------|-------------------|
| SepBIT | LOG_SEPBIT_FIFO_20260126_120619.csv | stat.log.20260126_120618 | replay_trace_sepbit.log |
| REFLASH_COLD_FIXED | LOG_GREEDY_COST_BENEFIT_COLD_20260127_000704.csv | stat.log.20260127_000703 | replay_trace_cold.log |
| REFLASH_WARM_FIXED | LOG_GREEDY_COST_BENEFIT_10_WARM_20260126_135917.csv | stat.log.20260126_135916 | replay_trace_warm_fixed.log |
| REFLASH | LOG_GREEDY_COST_BENEFIT_10_20260126_062111.csv | stat.log.20260126_062110 | replay_trace_fdp.log |
| CSAL | ftl0_20260126_110949.csv | N/A | replay_trace_ftl.log |
| OpenCAS | ocf0_20260126_082458.csv | N/A | replay_trace_ocf.log |

## 파일 경로
- CSV 파일: `ssd_waf/logging/`
- stat.log, replay_trace 파일: `ssd_waf/`

## CSV Column 매핑

### icache (SepBIT, REFLASH_*)
- host_write: `host_write_MB`
- tlc_write: `nvme_media_written_MB`
- qlc_write: `backend_write_MB`
- fdp_host_write: `nvme_host_written_MB`
- fdp_media_write: `nvme_media_written_MB`

### ftl (CSAL)
- host_write: `host_write_MB`
- tlc_write: `nvme_media_written_MB`
- qlc_write: `backend_write_MB`
- fdp_host_write: `nvme_host_written_MB`
- fdp_media_write: `nvme_media_written_MB`

### ocf (OpenCAS)
- host_write: `host_wr_MB`
- tlc_write: `nvme_media_MB`
- qlc_write: `core_wr_MB`
- fdp_host_write: `nvme_host_MB`
- fdp_media_write: `nvme_media_MB`

---

## Graph A: TLC vs QLC Writes (Scatter Plot)
- **타입**: 분산 그래프 (점 하나당 config 하나)
- **x축**: TLC writes (GB) - `nvme_media_written_MB`
- **y축**: QLC (evict) writes (GB) - `backend_write_MB`
- **단위**: GB
- **축 설정**: 원점 (0, 0) 기준

### 변경사항
- host write가 최종값에 도달한 첫 시점의 값을 사용 (flush 동작 제외)

---

## Graph B: Host Writes vs WAF (Time Series)
- **타입**: 시계열 꺾은선 그래프 (점 없이)
- **x축**: Host writes (GB), 500GB부터 시작
- **y축**: WAF = FDP media written / FDP host written, 0~7 범위
- **단위**: GB

### 변경사항
- ~~원래: y축 = TLC writes (GB)~~
- **변경 1**: y축을 WAF (TLC writes / host writes)로 변경
- **변경 2**: y축을 WAF (FDP media written / FDP host written)로 변경
- **변경 3**: x축 500GB부터 시작, y축 0~7 범위로 제한

---

## Graph C: Host Writes vs QLC Writes (Time Series)
- **타입**: 시계열 꺾은선 그래프
- **x축**: Host writes (GB)
- **y축**: QLC (evict) writes (GB)
- **단위**: GB
- **축 설정**: 원점 기준

### 변경사항
- **추가**: host write가 최종값에 도달한 첫 시점까지만 QLC writes 표시 (flush 동작 제외)

---

## Graph D: Normalized Cost (Bar Chart)
- **타입**: 세로 막대 그래프
- **x축**: Config
- **y축**: Normalized cost
- **Cost 공식**: `TLC writes + 6.73 * QLC writes`
- **Normalization**: CSAL 기준 (CSAL = 1.0)

### 변경사항
- host write가 최종값에 도달한 첫 시점의 값을 사용 (flush 동작 제외)

---

## Graph D-2: Actual Cost (Bar Chart)
- **타입**: 세로 막대 그래프
- **x축**: Config (레이블 없음)
- **y축**: Cost (GB)
- **Cost 공식**: `TLC writes + 6.73 * QLC writes`
- **참고**: Graph D와 동일한 공식이지만 정규화 없이 실제 값 표시

---

## Graph E: Evicted Ages Histogram
- **타입**: Histogram (config별 서브그래프 2x2)
- **x축**: Bucket (0-78) - 마지막 bucket 제외
- **y축**: Count
- **데이터**: stat.log의 마지막 `evicted_ages_with_segment`
- **대상 Config**: SepBIT, REFLASH_COLD_FIXED, REFLASH_WARM_FIXED, REFLASH

### 변경사항
- **추가**: 마지막 bucket (79번) 제외 (overflow bucket)

---

## Graph F: Compacted Ages Histogram
- **타입**: Histogram (config별 서브그래프 2x2)
- **x축**: Bucket (0-78) - 마지막 bucket 제외
- **y축**: Count
- **데이터**: stat.log의 마지막 `compacted_ages_with_segment`
- **대상 Config**: SepBIT, REFLASH_COLD_FIXED, REFLASH_WARM_FIXED, REFLASH

### 변경사항
- **추가**: 마지막 bucket (79번) 제외 (overflow bucket)

---

## Graph G: Throughput (Bar Chart)
- **타입**: 세로 막대 그래프
- **x축**: (레이블 없음)
- **y축**: Throughput (MB/s)
- **데이터**: replay_trace 파일의 마지막 "Average BW" 값

### 변경사항
- **추가**: x축 "Config" 레이블 삭제

---

## 공통 설정

### 폰트 크기
- 기본 폰트: 23
- 축 레이블: 24
- 제목: 26
- 틱 레이블: 20
- 범례: 18
- 막대 위 값 레이블: 18

### 축 설정
- 모든 그래프의 y축은 0부터 시작

### 색상 설정 (모든 그래프에서 동일)
| Config | 색상 | Hex Code |
|--------|------|----------|
| SepBIT | 파랑 | #1f77b4 |
| REFLASH_COLD_FIXED | 주황 | #ff7f0e |
| REFLASH_WARM_FIXED | 초록 | #2ca02c |
| REFLASH | 빨강 | #d62728 |
| CSAL | 보라 | #9467bd |
| OpenCAS | 시안 | #17becf |

### 데이터 처리
- host write가 최종값에 도달한 첫 시점의 값을 사용 (flush 동작 제외)
  - 마지막 host_write 값과 동일한 값을 가진 첫 번째 인덱스를 찾아서 해당 시점의 값 사용
- 모든 누적 값 (TLC, QLC, FDP)은 host write 시작 시점 기준 delta 값 사용
  - host_write > 0 인 첫 번째 시점의 값을 기준점으로 설정
  - 이후 모든 값에서 기준점을 빼서 delta 계산
  - 영향받는 그래프: A, B, C, D, D-2

---

## 출력 파일
- `graph_A_scatter.png`
- `graph_B_timeseries_tlc.png`
- `graph_C_timeseries_qlc.png`
- `graph_D_normalized_cost.png`
- `graph_D2_actual_cost.png`
- `graph_E_evicted_histogram.png`
- `graph_F_compacted_histogram.png`
- `graph_G_throughput.png`

## 스크립트 파일
- `plot_config.py`: 설정 파일 (파일 경로, 컬럼 매핑 등)
- `plot_graphs.py`: 그래프 생성 스크립트
