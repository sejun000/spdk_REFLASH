#pragma once
#include <list>
#include <cstdint>

class Segment;

class EvictPolicy {
public:
    virtual ~EvictPolicy() = default;

    /* victim 선택 */
    virtual Segment* choose_segment() = 0;
    virtual Segment* choose_segment(bool next_id) {
        return choose_segment();  // 기본 구현은 next_id 여부 무시
    }
    virtual Segment* choose_segment(int stream_id) {
        return choose_segment();  // 기본 구현은 stream_id 무시
    }

    // only for do_evict_and_compaction_with_same_policy
    virtual Segment* choose_segment_for_eviction(bool next_id) {
        return choose_segment();
    }

    virtual Segment* choose_segment_for_compaction(bool next_id) {
        return choose_segment();
    }

    /* 세그먼트 추가 / 제거 */
    virtual void add(Segment* seg)       = 0;
    virtual void add(Segment* seg, uint64_t current_time) { 
        add(seg);  // 기본 구현은 current_time 무시 
    }

    virtual void remove(Segment* seg)    = 0;

    /* valid_cnt 가 변했을 때 호출 */
    virtual void update(Segment* seg)    = 0;
    virtual void update(Segment* seg, uint64_t current_time) { 
        update(seg);  // 기본 구현은 current_time 무시 
    }
    void init(uint64_t* time, std::size_t size, int num) {
        logical_time = time;  // 외부에서 logical_time을 설정
        pages_in_segment = size;  // 세그먼트 크기 설정
        segment_num = num;  // 세그먼트 개수 설정
    }
    // protected: // EvictPolicy는 Segment에 대한 접근이 필요하므로 protected로 설정
protected:
    std::size_t pages_in_segment = 0;  // 세그먼트 내의 page수 
    uint64_t segment_num = 0;          // 세그먼트 개수
    uint64_t* logical_time = nullptr;  // 전역 증가하는 logical time
};
