#include "zm_util_container.h"

#include <assert.h>
#include <chrono>
#include <stdarg.h>
#include <thread>

// ============================================================================
// ZmByteBuffer
// ============================================================================

// --- 构造与析构 ---

ZmByteBuffer::ZmByteBuffer(size_t size, const void* data) : m_capacity(0), m_head(NULL)
{
    m_size = ZM_MAX(1, size);
    XAlloc(m_size);
    if (data && size > 0 && m_head)
    {
        memcpy(m_head, data, size);
    }
}

ZmByteBuffer::~ZmByteBuffer()
{
    if (m_head)
    {
        free(m_head);
    }
    m_head = NULL;
}

// --- 容量与状态 ---

size_t ZmByteBuffer::Size()
{
    return m_size;
}

size_t ZmByteBuffer::Capacity()
{
    return m_capacity;
}

// --- 数据访问 ---

unsigned char* ZmByteBuffer::Head(size_t offset)
{
    return m_head + offset;
}

char* ZmByteBuffer::Str(size_t offset)
{
    return (char*)(m_head + offset);
}

uint32_t ZmByteBuffer::GetUINT32(size_t offset)
{
    return (m_size >= (offset + 4)) ? *((uint32_t*)(m_head + offset)) : 0;
}

unsigned char& ZmByteBuffer::operator[](const size_t index)
{
    return m_head[index];
}

// --- 数据写入 ---

void ZmByteBuffer::Put(const void* buf, size_t count, size_t offset)
{
    if (offset >= m_size) { return; }
    memcpy(m_head + offset, buf, ZM_MIN(count, m_size - offset));
}

void ZmByteBuffer::Sprintf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);

    va_list vtmp;
    va_copy(vtmp, ap);
    size_t dlen = vsnprintf(NULL, 0, fmt, vtmp);
    Expand(dlen + 1);
    va_end(vtmp);

    vsnprintf((char*)m_head, m_size, fmt, ap);
    va_end(ap);
}

// --- 数据搬移与重置 ---

void ZmByteBuffer::Compact(size_t offset, size_t remains)
{
    if ((offset + remains) <= m_size)
    {
        if (remains > 0)
        {
            memmove(m_head, m_head + offset, remains);
        }
        memset(m_head + remains, 0, m_size - remains);
    }
}

void ZmByteBuffer::Expand(size_t newsize)
{
    if (newsize > m_size)
    {
        XAlloc(newsize, true);
    }
    else if (newsize < m_size)
    {
        memset(m_head + newsize, 0, m_size - newsize);
    }
    m_size = newsize;
}

void ZmByteBuffer::Reset(size_t size, const void* data)
{
    XAlloc(size);
    Zero();
    m_size = size;
    if (NULL != data)
    {
        memcpy(m_head, data, m_size);
    }
}

// --- 清零与清空 ---

void ZmByteBuffer::Zero(size_t offset)
{
    if (m_head && offset < m_capacity)
    {
        size_t size = m_capacity - offset + 1;
        memset(m_head + offset, 0, size);
    }
}

void ZmByteBuffer::Clean()
{
    m_size = m_capacity;
    memset(m_head, 0, m_size + 1);
}

// --- 内部实现 ---

void ZmByteBuffer::XAlloc(size_t capacity, bool remain)
{
    if (capacity > m_capacity)
    {
        unsigned char* old = m_head;

        while (true) {
            m_head = (unsigned char*)malloc(capacity + 1);
            if (m_head) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        memset(m_head, 0, capacity + 1);
        if (old)
        {
            if (remain)
            {
                assert(m_size <= capacity);
                memcpy(m_head, old, m_size);
            }
            free(old);
        }
        m_capacity = capacity;
    }
}
