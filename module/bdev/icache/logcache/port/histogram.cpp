#include "histogram.h"
#include <stdexcept>

Histogram::Histogram(std::string name, uint64_t granularity, size_t max_buckets, FILE *fp_histo)
    : m_name(name), m_granularity(granularity), m_max_buckets(max_buckets), m_fp_histo(fp_histo) {
    if (m_granularity == 0) {
        m_granularity = 1;
    }
    if (m_fp_histo == nullptr) {
        throw std::invalid_argument("Histogram received a null file pointer.");
    }
    // max_buckets가 0이면 의미가 없으므로 예외 처리
    if (m_max_buckets == 0) {
        throw std::invalid_argument("max_buckets must be greater than 0.");
    }

    // m_counts 벡터를 m_max_buckets 크기만큼 할당하고 모든 값을 0으로 초기화
    m_counts.resize(m_max_buckets, 0);
}

Histogram::~Histogram() {
    if (m_fp_histo) {
        // 배열의 처음부터 끝까지 순회
        fprintf(m_fp_histo, "---summary of %s---\n", m_name.c_str());
        for (size_t i = 0; i < m_max_buckets; ++i) {
            // 카운트가 0보다 큰 버킷만 출력
            if (m_counts[i] > 0) {
                // %zu는 size_t 타입에 대한 형식 지정자
                fprintf(m_fp_histo, "%zu th %lu\n", i, m_counts[i]);
            }
        }
        fflush(m_fp_histo);
    }
}

void Histogram::inc(uint64_t key, int inc) {
    uint64_t index = key / m_granularity;
    // 계산된 인덱스가 배열의 크기를 벗어나는지 확인
    if (index >= m_max_buckets) {
        // 벗어난다면 가장 마지막 인덱스에 카운트를 누적
        index = m_max_buckets - 1;
    }

    m_counts[index] += inc;
}