#pragma once
#include <vector>
#include <cstddef>
#include <cstdint>

/**
 * 한 세그먼트(≈32 MB)를 구성하는 내부 자료구조
 */
class Segment
{
public:
    Segment(uint64_t create_time) {
        create_timestamp = create_time;
    }
    /* data */
    std::size_t        write_ptr = 0;
    std::size_t        valid_cnt = 0;
    int class_num = 0;
    bool hot = false;
    uint64_t create_timestamp;
    /* helpers */
    virtual bool full() = 0;
    virtual void reset() = 0;
    virtual uint64_t get_create_time() {
        return create_timestamp;
    }
    virtual int get_class_num() {
        return class_num;
    }
    static const int GC_STREAM_START = 10;
};
