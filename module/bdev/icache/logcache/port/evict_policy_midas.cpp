#include "evict_policy_midas.h"
#include <algorithm>

MiDASGreedyEvictPolicy::MiDASGreedyEvictPolicy(std::size_t stream_count)
    : queues_(std::max<std::size_t>(1, stream_count)),
      generation_counter_(0),
      next_stream_(0) {}

void MiDASGreedyEvictPolicy::add(Segment* seg) {
    if (!seg) return;
    std::size_t stream = normalise_stream(seg->get_class_num());
    push_entry(seg, stream);
}

void MiDASGreedyEvictPolicy::remove(Segment* seg) {
    if (!seg) return;
    version_.erase(seg);
}

void MiDASGreedyEvictPolicy::update(Segment* seg) {
    if (!seg) return;
    std::size_t stream = normalise_stream(seg->get_class_num());
    push_entry(seg, stream);
}

Segment* MiDASGreedyEvictPolicy::choose_segment() {
    if (queues_.empty()) return nullptr;

    const std::size_t total_streams = queues_.size();
    for (std::size_t attempt = 0; attempt < total_streams; ++attempt) {
        std::size_t stream = (next_stream_ + attempt) % total_streams;
        auto& queue = queues_[stream];

        while (!queue.empty()) {
            const QueueEntry entry = queue.top();
            auto it = version_.find(entry.seg);
            if (it == version_.end() || it->second.first != entry.generation || it->second.second != stream) {
                queue.pop();
                continue;
            }
            queue.pop();
            next_stream_ = (stream + 1) % total_streams;
            return entry.seg;
        }
    }

    return nullptr;
}

std::size_t MiDASGreedyEvictPolicy::normalise_stream(int class_num) const {
    if (queues_.empty()) return 0;
    if (class_num < 0) return 0;
    std::size_t stream = static_cast<std::size_t>(class_num);
    if (stream >= queues_.size()) {
        stream %= queues_.size();
    }
    return stream;
}

void MiDASGreedyEvictPolicy::push_entry(Segment* seg, std::size_t stream) {
    if (queues_.empty()) return;
    generation_counter_++;
    version_[seg] = std::make_pair(generation_counter_, stream);
    queues_[stream].push(QueueEntry{
        static_cast<uint64_t>(seg->valid_cnt),
        generation_counter_,
        seg,
        static_cast<int>(stream)
    });
}
