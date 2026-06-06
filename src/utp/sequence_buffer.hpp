#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <cassert>
#include <cstddef>

namespace utp {

// 原始序列号环形缓冲区
// 设计原理：使用环形缓冲区 + 位掩码实现 O(1) 的序列号映射
// 缓冲区大小总是 2 的幂，通过序列号与掩码进行快速索引
// 用于管理有序数据包的接收和重排序
class RawSequenceBuffer {
    std::vector<void*> buf_;  // 数据缓冲区
    size_t mask_ = 0;         // 位掩码 (大小-1，用于环形索引)

public:
    RawSequenceBuffer() = default;

    // 获取指定序列号的数据
    void* get(size_t seq) const {
        if (mask_ == 0) return nullptr;
        return buf_[seq & mask_];
    }

    // 存储指定序列号的数据
    void put(size_t seq, void* data) {
        assert(buf_.size() > 0);
        buf_[seq & mask_] = data;
    }

    // 确保缓冲区足够大以容纳指定索引
    void ensure_size(size_t item, size_t index) {
        if (index > mask_) grow(item, index);
    }

    size_t size() const { return mask_ + 1; }  // 获取缓冲区大小
    size_t mask() const { return mask_; }       // 获取位掩码

    // 扩展缓冲区到足够容纳指定索引
    // 新大小为 2 的幂，且 >= index + 1
    void grow(size_t item, size_t index) {
        size_t new_size = mask_ + 1;
        if (new_size == 0) new_size = 1;
        do new_size *= 2; while (index >= new_size);

        std::vector<void*> new_buf(new_size, nullptr);
        size_t new_mask = new_size - 1;

        // 迁移现有数据到新缓冲区，保持正确的环形偏移
        if (mask_ > 0 || buf_.size() > 0) {
            for (size_t i = 0; i <= mask_; i++) {
                new_buf[(item - index + i) & new_mask] = buf_[(item - index + i) & mask_];
            }
        }

        buf_ = std::move(new_buf);
        mask_ = new_mask;
    }

    // 直接元素访问 (用于清理)
    void*& element(size_t idx) { return buf_[idx & mask_]; }
    const void* element(size_t idx) const { return buf_[idx & mask_]; }
    size_t buf_size() const { return buf_.size(); }

    // 初始化为指定大小 (用于向后兼容)
    void initialize(size_t size) {
        buf_.assign(size, nullptr);
        mask_ = size - 1;
    }
};

}  // namespace utp