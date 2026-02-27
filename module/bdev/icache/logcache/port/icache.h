#pragma once
#include <map>
#include <string>
#include <tuple>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <iostream>
#include "common.h"

class ICache {
public:
    virtual ~ICache();
    ICache(uint64_t cold_capacity, const std::string& waf_log_file, const std::string& stat_log_file = "");
    std::string get_timestamp() {
        auto     now   = std::chrono::system_clock::now();
        std::time_t tt = std::chrono::system_clock::to_time_t(now);

        // ② tm 구조체로 변환 (스레드 안전 함수 사용 – POSIX: localtime_r, Windows: localtime_s)
        std::tm tm {};
        localtime_r(&tt, &tm);        // Linux/Unix
        // localtime_s(&tm, &tt);     // Windows라면 이 줄로 교체

        // ③ 원하는 형식으로 문자열화
        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y%m%d_%H%M%S");   // 예: 20250728_113014
        std::string timestamp = oss.str();
        return timestamp;
    }
    virtual bool exists(long key) = 0;
    virtual void touch(long key, OP_TYPE op_type) = 0;
    virtual void batch_insert(int stream_id, const std::map<long, int> &newBlocks, OP_TYPE op_type) = 0;
    virtual bool is_cache_filled() = 0;
    virtual int get_block_size() = 0;
    virtual void print_cache_trace(long long lba_offset, int lba_size, OP_TYPE op_type){};
    void _evict_one_block(uint64_t lba_offset, int lba_size, OP_TYPE op_type);
    void _invalidate_cold_block(uint64_t lba_offset, int lba_size, OP_TYPE op_type);
    virtual void print_stats() {}
    virtual void evict_one_block() = 0;
    virtual size_t size() = 0;
    virtual bool is_no_cache() { return false; }
    std::tuple<long long, long long, long long> get_status();
    void set_stats_prefix(const std::string& prefix);
    const std::string& stats_prefix() const;
    long long write_size_to_cache;
    long long evicted_blocks;
    long long write_hit_size;
    long long next_write_size_to_cache;
    FILE *fp;
    FILE *fp_stats = nullptr;
    FILE *fp_object = nullptr;
    FILE *fp_victim_ratio = nullptr;
protected:
    std::string stats_prefix_;
};

ICache* createCache(std::string cache_type, long capacity, uint64_t cold_capacity,
                    int cache_block_size, bool _cache_trace, const std::string &trace_file,
                    const std::string &cold_trace_file, std::string &waf_log_file,
                    double valid_rate_threshold = 0.0, std::string stat_log_file = "");
