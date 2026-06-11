#pragma once
#include <cstdint>
#include <vector>
#include <memory>
#include <cassert>
#include <cstddef>

namespace utp {

// 序列号环形缓冲区（拥有元素所有权）
// 设计原理：使用环形缓冲区 + 位掩码实现 O(1) 的序列号映射
// 缓冲区大小总是 2 的幂，通过序列号与掩码进行快速索引
// 用于管理有序数据包的接收和重排序
// 元素以 unique_ptr<T> 持有：覆盖/析构时自动释放，无需调用方手动 delete
template <typename T>
class SequenceBuffer {
    std::vector<std::unique_ptr<T>> buf_;  // 数据缓冲区
    size_t mask_ = 0;                      // 位掩码 (大小-1，用于环形索引)

public:
    SequenceBuffer() = default;

    // 观察指定序列号的元素（不转移所有权）
    T* get(size_t seq) const {
        if (mask_ == 0) return nullptr;
        return buf_[seq & mask_].get();
    }

    // 存入指定序列号的元素（旧元素若存在则被释放）
    void put(size_t seq, std::unique_ptr<T> data) {
        assert(buf_.size() > 0);
        buf_[seq & mask_] = std::move(data);
    }

    // 取走指定序列号的元素所有权，槽位置空
    std::unique_ptr<T> take(size_t seq) {
        if (mask_ == 0) return nullptr;
        return std::move(buf_[seq & mask_]);
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

        std::vector<std::unique_ptr<T>> new_buf(new_size);
        size_t new_mask = new_size - 1;

        // 迁移现有数据到新缓冲区，保持正确的环形偏移
        if (mask_ > 0 || buf_.size() > 0) {
            for (size_t i = 0; i <= mask_; i++) {
                new_buf[(item - index + i) & new_mask] = std::move(buf_[(item - index + i) & mask_]);
            }
        }

        buf_ = std::move(new_buf);
        mask_ = new_mask;
    }

    // 初始化为指定大小（清空已有元素）
    void initialize(size_t size) {
        buf_.clear();
        buf_.resize(size);
        mask_ = size - 1;
    }
};

}  // namespace utp
