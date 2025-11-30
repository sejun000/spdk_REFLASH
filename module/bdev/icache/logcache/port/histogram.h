#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector> // std::map 대신 std::vector 사용

class Histogram {
public:
    /**
     * @brief histogram 생성자
     * @param granularity 키를 인덱스로 변환하기 위한 단위 값
     * @param max_buckets 생성할 히스토그램 버킷의 최대 개수
     * @param fp_histo 히스토그램 결과를 출력할 파일 포인터
     */
    Histogram(std::string name, uint64_t granularity, size_t max_buckets, FILE *fp_histo);

    /**
     * @brief histogram 소멸자
     * 객체가 소멸될 때, 저장된 히스토그램 데이터를 fp_histo 파일에 출력합니다.
     */
    ~Histogram();

    /**
     * @brief 특정 키에 해당하는 히스토그램의 카운트를 증가시킵니다.
     * @param key 카운트를 증가시킬 대상 키
     * @param inc 증가시킬 값 (기본값: 1)
     */
    void inc(uint64_t key, int inc = 1);

private:
    std::string m_name; // 히스토그램 이름
    uint64_t m_granularity;
    size_t m_max_buckets;                       // 최대 버킷 개수 저장
    FILE* m_fp_histo;
    std::vector<uint64_t> m_counts;             // 고정 크기 배열로 데이터 저장
};
