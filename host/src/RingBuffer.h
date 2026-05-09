//=============================================================================
// RingBuffer.h — 线程安全预分配环形缓冲 (header-only)
//
// 追加不分配内存，专为高速波形数据暂存设计。
// 写入线程: DataProcessor (QThread)
// 读取线程: MainWindow (GUI, QTimer 33ms)
//
// 线程安全: atomic writePos + 单写单读模式
//=============================================================================
#pragma once
#include <vector>
#include <atomic>
#include <span>
#include <algorithm>
#include <cstddef>

template<typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity)
        : m_buffer(capacity), m_capacity(capacity), m_writePos(0)
    {
    }

    //=====================================================================
    // 追加数据块 (写线程调用)
    //=====================================================================
    void append(std::span<const T> data)
    {
        if (data.empty()) return;
        size_t len = data.size();
        if (len > m_capacity) {
            // 只保留最后 capacity 个元素
            data = data.subspan(len - m_capacity);
            len = m_capacity;
        }

        size_t writeIdx = m_writePos.load(std::memory_order_relaxed) % m_capacity;
        size_t firstPart = m_capacity - writeIdx;

        if (len <= firstPart) {
            std::copy(data.begin(), data.end(), m_buffer.begin() + static_cast<long>(writeIdx));
        } else {
            std::copy(data.begin(), data.begin() + static_cast<long>(firstPart),
                      m_buffer.begin() + static_cast<long>(writeIdx));
            std::copy(data.begin() + static_cast<long>(firstPart), data.end(),
                      m_buffer.begin());
        }
        m_writePos.fetch_add(len, std::memory_order_release);
    }

    /// 追加单个元素
    void append(const T &value)
    {
        size_t writeIdx = m_writePos.load(std::memory_order_relaxed) % m_capacity;
        m_buffer[writeIdx] = value;
        m_writePos.fetch_add(1, std::memory_order_release);
    }

    //=====================================================================
    // 获取最近 N 个数据点 (读线程调用, 只读)
    //=====================================================================
    [[nodiscard]] std::vector<T> getRecent(size_t count) const
    {
        size_t pos = m_writePos.load(std::memory_order_acquire);

        if (pos == 0)
            return {};

        if (count > pos)
            count = pos;
        if (count > m_capacity)
            count = m_capacity;
        if (count == 0)
            return {};

        size_t start = (pos - count) % m_capacity;
        std::vector<T> result(count);

        if (start + count <= m_capacity) {
            std::copy(m_buffer.begin() + static_cast<long>(start),
                      m_buffer.begin() + static_cast<long>(start + count),
                      result.begin());
        } else {
            size_t firstPart = m_capacity - start;
            std::copy(m_buffer.begin() + static_cast<long>(start),
                      m_buffer.end(), result.begin());
            std::copy(m_buffer.begin(),
                      m_buffer.begin() + static_cast<long>(count - firstPart),
                      result.begin() + static_cast<long>(firstPart));
        }
        return result;
    }

    //=====================================================================
    // 统计
    //=====================================================================
    [[nodiscard]] size_t totalWritten() const
    {
        return m_writePos.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t capacity() const { return m_capacity; }

    [[nodiscard]] size_t size() const
    {
        size_t total = m_writePos.load(std::memory_order_acquire);
        return (total > m_capacity) ? m_capacity : total;
    }

    [[nodiscard]] bool empty() const
    {
        return m_writePos.load(std::memory_order_acquire) == 0;
    }

    /// 清空 (仅重置写指针, 不释放内存)
    void clear()
    {
        m_writePos.store(0, std::memory_order_release);
    }

private:
    std::vector<T>       m_buffer;
    size_t               m_capacity;
    std::atomic<size_t>  m_writePos;
};
