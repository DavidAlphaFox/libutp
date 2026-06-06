#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <cassert>
#include <cstddef>

namespace utp {

// RawSequenceBuffer - a drop-in replacement for SizableCircularBuffer
// that uses std::vector<void*> internally instead of raw malloc'd array.
// This preserves the existing API for Phase 4 migration; unique_ptr
// conversion happens in Phase 5.
class RawSequenceBuffer {
    std::vector<void*> buf_;
    size_t mask_ = 0;

public:
    RawSequenceBuffer() = default;

    void* get(size_t seq) const {
        if (mask_ == 0) return nullptr;
        return buf_[seq & mask_];
    }

    void put(size_t seq, void* data) {
        assert(buf_.size() > 0);
        buf_[seq & mask_] = data;
    }

    void ensure_size(size_t item, size_t index) {
        if (index > mask_) grow(item, index);
    }

    size_t size() const { return mask_ + 1; }
    size_t mask() const { return mask_; }

    void grow(size_t item, size_t index) {
        size_t new_size = mask_ + 1;
        if (new_size == 0) new_size = 1;
        do new_size *= 2; while (index >= new_size);

        std::vector<void*> new_buf(new_size, nullptr);
        size_t new_mask = new_size - 1;

        if (mask_ > 0 || buf_.size() > 0) {
            for (size_t i = 0; i <= mask_; i++) {
                new_buf[(item - index + i) & new_mask] = buf_[(item - index + i) & mask_];
            }
        }

        buf_ = std::move(new_buf);
        mask_ = new_mask;
    }

    // Direct element access for cleanup (returns reference to allow free())
    void*& element(size_t idx) { return buf_[idx & mask_]; }
    const void* element(size_t idx) const { return buf_[idx & mask_]; }
    size_t buf_size() const { return buf_.size(); }

    // Initialize with a specific size (for legacy compatibility)
    void initialize(size_t size) {
        buf_.assign(size, nullptr);
        mask_ = size - 1;
    }
};

}  // namespace utp