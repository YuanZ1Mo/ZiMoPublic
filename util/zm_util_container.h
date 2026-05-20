#ifndef ZM_UTIL_CONTAINER_HPP
#define ZM_UTIL_CONTAINER_HPP

#include "../define/zm_simple_define.h"

#include <functional>
#include <stack>
#include <unordered_set>

/**
 * @brief 动态字节缓冲区，用于网络 I/O 等场景的读写缓冲
 *
 * 管理一块连续的堆内存，支持按偏移读写、格式化写入、扩容、数据搬移等操作。
 * 内部分配时多分配 1 字节（m_capacity+1），用于防止越界访问时的零终止。
 */
class ZmByteBuffer
{
public:
    /**
     * @brief 构造并分配指定大小的缓冲区
     * @param size  期望的数据大小（字节），实际至少分配 1 字节
     * @param data  可选，初始化时拷贝的源数据
     */
    ZmByteBuffer(size_t size = 0, const void* data = NULL);

    /**
     * @brief 析构，释放缓冲区内存
     */
    ~ZmByteBuffer();

    // --- 容量与状态 ---

    /**
     * @brief 获取缓冲区的逻辑大小
     * @return 当前有效数据大小（字节数）
     */
    size_t Size();

    /**
     * @brief 获取缓冲区的实际分配容量
     * @return 容量大小（字节数）
     */
    size_t Capacity();

    // --- 数据访问 ---

    /**
     * @brief 获取缓冲区数据指针（带偏移）
     * @param offset  偏移量（默认 0）
     * @return 指向 m_head + offset 的指针
     */
    unsigned char* Head(size_t offset = 0);

    /**
     * @brief 获取缓冲区数据的字符串表示
     * @param offset  偏移量（默认 0）
     * @return 指向 m_head + offset 的 char* 指针
     */
    char* Str(size_t offset = 0);

    /**
     * @brief 从缓冲区指定偏移处读取一个 32 位无符号整数
     * @param offset  读取起始偏移（默认 0）
     * @return 读取到的 uint32_t 值；若剩余空间不足 4 字节则返回 0
     */
    uint32_t GetUINT32(size_t offset = 0);

    /**
     * @brief 按索引访问缓冲区中的单个字节
     * @param index  字节索引
     * @return 对应位置的字节引用
     */
    unsigned char& operator[](const size_t index);

    // --- 数据写入 ---

    /**
     * @brief 将外部数据写入缓冲区的指定偏移位置
     * @param buf     源数据指针
     * @param count   要写入的字节数
     * @param offset  写入起始偏移（默认 0）
     * @note 实际写入量为 min(count, m_size - offset)，不会越界写入
     */
    void Put(const void* buf, size_t count, size_t offset = 0);

    /**
     * @brief 将格式化字符串写入缓冲区（自动扩容）
     * @param fmt  printf 风格的格式字符串
     * @param ...  可变参数
     * @note 先用 vsnprintf(NULL,0,...) 计算所需长度，再 Expand 到对应大小后写入
     */
    void Sprintf(const char* fmt, ...);

    // --- 数据搬移与重置 ---

    /**
     * @brief 将 [offset, offset+remains) 范围的数据搬移到缓冲区头部，尾部清零
     * @param offset   要搬移的数据起始偏移
     * @param remains  要搬移的字节数
     *
     * 典型用途：网络缓冲区中已消费掉 offset 之前的字节后，
     * 将未消费的 remains 字节移到头部，类似 Java NIO 的 Buffer.compact()。
     */
    void Compact(size_t offset, size_t remains);

    /**
     * @brief 调整缓冲区的逻辑大小
     * @param newsize  新的逻辑大小
     * @note 扩大时自动分配更大内存并保留原有数据；缩小时仅将尾部清零
     */
    void Expand(size_t newsize);

    /**
     * @brief 重置缓冲区为指定大小，并可选地用源数据初始化
     * @param size  新的大小
     * @param data  可选，要拷贝的初始数据
     */
    void Reset(size_t size, const void* data = NULL);

    // --- 清零与清空 ---

    /**
     * @brief 将从 offset 开始到缓冲区末尾的全部字节清零
     * @param offset  起始偏移（默认 0，即清零整个缓冲区）
     */
    void Zero(size_t offset = 0);

    /**
     * @brief 清空整个缓冲区（将 m_size 恢复为 m_capacity，全部字节清零）
     */
    void Clean();

private:
    /**
     * @brief 内部内存分配器
     * @param capacity  请求的容量
     * @param remain    为 true 时保留原有数据到新内存中
     * @note 当请求容量 > 当前 m_capacity 时才实际分配；
     *       malloc 失败时会无限重试（每 100ms），不会返回 NULL
     */
    void XAlloc(size_t capacity, bool remain = false);

private:
    size_t          m_capacity;     /* 已分配的总容量（字节数） */
    size_t          m_size;         /* 当前逻辑大小（字节数），<= m_capacity */
    unsigned char*  m_head;         /* 缓冲区首地址 */
};

/**
 * @brief 基于 POD 内存拷贝的动态数组
 *
 * @tparam T  元素类型，必须是可平凡拷贝的（POD），因为内部使用 memcpy/memmove
 *
 * 类似 std::vector 的轻量替代，支持尾部追加、按位置插入/删除、自动扩容。
 * 扩容策略：每次扩容 max(2, capacity/4)，即约 25% 增量且最少增加 2 个槽位。
 */
template<typename T>
class ZmArrayList
{
public:
    enum { ELEMENT_SIZE = sizeof(T) };

    /**
     * @brief 构造并预分配指定容量的数组
     * @param capacity  初始容量（默认 32）
     */
    ZmArrayList(size_t capacity = 32)
    {
        m_count = 0;
        m_capacity = capacity;
        Alloc();
    }

    /**
     * @brief 析构，释放内部存储
     */
    ~ZmArrayList()
    {
        if (NULL != m_items)
        {
            free(m_items);
        }
        m_items = NULL;
        m_count = 0;
    }

    // --- 容量与状态 ---

    /**
     * @brief 获取当前元素数量
     * @return 元素个数
     */
    size_t Count() { return m_count; }

    /**
     * @brief 获取当前容量
     * @return 容量大小
     */
    size_t Capacity() { return m_capacity; }

    /**
     * @brief 判断数组是否已满（m_count >= m_capacity）
     * @return true 表示已满，下次 Add/Insert 前需要扩容
     */
    bool IsFull() { return (m_count >= m_capacity); }

    // --- 数据访问 ---

    /**
     * @brief 获取底层数组首地址
     * @return 指向第一个元素的指针
     */
    T* Head() { return m_items; }

    /**
     * @brief 按索引获取元素指针
     * @param pos  索引位置
     * @return 对应元素的指针，越界时返回 NULL
     */
    T* At(size_t pos) { return (pos < m_count) ? (m_items + pos) : NULL; }

    /**
     * @brief 获取最后一个元素的指针
     * @return 最后一个元素的指针，数组为空时返回 NULL
     */
    T* Last() { return m_count > 0 ? (m_items + (m_count - 1)) : NULL; }

    /**
     * @brief 按索引访问元素（等价于 At）
     * @param pos  索引位置
     * @return 对应元素的指针
     */
    T* operator[](size_t pos) { return At(pos); }

    // --- 元素操作 ---

    /**
     * @brief 在数组末尾追加一个元素
     * @param item  要拷贝的源元素指针，为 NULL 时追加一个零初始化的元素
     * @return 指向新追加元素的指针
     */
    T* Add(const T* item = NULL)
    {
        if (IsFull())
        {
            Expand();
        }
        T* p = m_items + m_count;
        if (NULL != item)
        {
            memcpy(p, item, ELEMENT_SIZE);
        }
        else
        {
            memset(p, 0, ELEMENT_SIZE);
        }
        m_count++;
        return p;
    }

    /**
     * @brief 在指定位置插入一个元素，后续元素右移
     * @param pos   插入位置，超出范围时自动夹紧到 [0, m_count]
     * @param item  要拷贝的源元素指针，为 NULL 时插入零初始化的元素
     * @return 指向新插入元素的指针
     */
    T* Insert(size_t pos, T* item = NULL)
    {
        if (IsFull())
        {
            Expand();
        }
        pos = ZM_MIN(m_count, ZM_MAX(pos, 0));
        memmove(m_items + pos + 1, m_items + pos, ELEMENT_SIZE * (m_count - pos));
        T* p = m_items + pos;
        if (NULL != item)
        {
            memcpy(p, item, ELEMENT_SIZE);
        }
        else
        {
            memset(p, 0, ELEMENT_SIZE);
        }
        m_count++;
        return p;
    }

    /**
     * @brief 移除指定位置的元素，后续元素左移填补
     * @param pos  要移除的元素索引
     */
    void Remove(size_t pos)
    {
        if (pos < m_count)
        {
            T* ptr = m_items + pos;
            memmove(ptr, ptr + 1, ELEMENT_SIZE * (m_count - pos - 1));
            memset(m_items + m_count - 1, 0, ELEMENT_SIZE);
            m_count--;
        }
    }

    /**
     * @brief 根据元素指针反算其在数组中的索引
     * @param item  指向数组内某个元素的指针
     * @return 元素的索引位置（字节偏移量 / 元素大小）
     */
    int OffsetOf(T* item)
    {
        int offset = (int)((char*)item - (char*)m_items);
        return offset / ELEMENT_SIZE;
    }

    /**
     * @brief 交换两个位置的元素
     * @param p1  第一个位置的索引
     * @param p2  第二个位置的索引
     */
    void Swap(size_t p1, size_t p2)
    {
        if (p1 != p2)
        {
            T tmp;
            memcpy(&tmp, m_items + p1, ELEMENT_SIZE);
            memcpy(m_items + p1, m_items + p2, ELEMENT_SIZE);
            memcpy(m_items + p2, &tmp, ELEMENT_SIZE);
        }
    }

    /**
     * @brief 按索引设置元素的值
     * @param pos  索引位置
     * @param val  要拷贝的源数据指针
     */
    void SetAt(size_t pos, T* val)
    {
        if (pos < m_count)
        {
            memcpy(m_items + pos, val, ELEMENT_SIZE);
        }
    }

    // --- 清空 ---

    /**
     * @brief 清空数组（m_count 归零，内存内容全部清零，但不释放内存）
     */
    void Clear()
    {
        m_count = 0;
        memset(m_items, 0, ELEMENT_SIZE * m_capacity);
    }

protected:
    /**
     * @brief 扩容内部数组
     *
     * 新容量 = 当前容量 + max(2, 当前容量/4)。
     * 分配新内存后将原有 m_count 个元素拷贝过来，释放旧内存。
     */
    void Expand()
    {
        size_t capacityx = ZM_MAX(m_count, m_capacity);
        m_capacity = capacityx + ZM_MAX(2, capacityx / 4);

        T* tmp = m_items;
        Alloc();
        memcpy(m_items, tmp, ELEMENT_SIZE * m_count);
        free(tmp);
    }

    /**
     * @brief 分配 m_capacity + 1 个元素的内存并全部清零
     * @note 多分配 1 个元素用于二分查找等场景的越界防护
     */
    void Alloc()
    {
        m_items = (T*)malloc(ELEMENT_SIZE * (m_capacity + 1));
        if (m_items)
        {
            memset(m_items, 0, ELEMENT_SIZE * (m_capacity + 1));
        }
    }

private:
    size_t  m_capacity;     /* 当前已分配的容量（元素个数） */
    size_t  m_count;        /* 当前实际元素个数 */
    T*      m_items;        /* 底层连续内存 */
};

/**
 * @brief 固定大小对象的内存池，避免频繁的 malloc/free
 *
 * @tparam T           对象类型，必须是 POD（池内使用 memset 清零）
 * @tparam ObjectSize  单个对象的大小（字节），默认为 sizeof(T)；
 *                     可显式指定以预留额外空间或控制对齐
 *
 * 以 chunk 为单位批量分配内存，每个 chunk 包含 m_chunk_size 个对象槽位。
 * 对象通过 Apply() 获取、Release() 归还，归还后可被再次复用。
 * 内部使用 unordered_set 防止同一个对象被重复 Release。
 */
template<typename T, size_t ObjectSize = sizeof(T)>
class ZmObjectPool
{
public:
    /** 16 字节对齐后的元素大小 */
    enum { ELEMENT_SIZE = ((ObjectSize + 0x0F) & ~(size_t)0xF) };

    /**
     * @brief 构造对象池
     * @param chunk_size  每个 chunk 包含的对象槽位数（最少 4）
     */
    ZmObjectPool(size_t chunk_size = 8)
    {
        m_chunk_size = ZM_MAX(chunk_size, 4);
        /* m_shift 用于 HashKey：将指针右移 m_shift 位作为哈希值，
           m_shift = log2(ELEMENT_SIZE+1)，使得相邻对象的哈希值不同 */
        m_shift = zm_log2(1 + ELEMENT_SIZE);
    }

    /**
     * @brief 析构，释放所有 chunk 内存
     */
    ~ZmObjectPool()
    {
        FreeChunks();
    }

    // --- 对象申请与归还 ---

    /**
     * @brief 从池中申请一个对象
     * @return 指向可用对象的指针（已零初始化）
     *
     * 若空闲栈为空，则分配一个新 chunk（含 m_chunk_size 个对象槽位），
     * 全部压入空闲栈后再弹出第一个返回。
     */
    T* Apply()
    {
        if (m_idles.empty())
        {
            /* 分配新 chunk 并将其中每个槽位压入空闲栈 */
            void* chunk = malloc(ELEMENT_SIZE * m_chunk_size);
            memset(chunk, 0, ELEMENT_SIZE * m_chunk_size);
            m_chunks.push_back(chunk);

            unsigned char* ptr = (unsigned char*)chunk;
            for (size_t i = 0; i < m_chunk_size; i++)
            {
                m_idle_key_set.insert(HashKey(ptr));
                m_idles.push((T*)ptr);
                ptr += ELEMENT_SIZE;
            }
        }
        T* obj = m_idles.top();
        m_idles.pop();
        m_idle_key_set.erase(m_idle_key_set.find(HashKey(obj)));
        return obj;
    }

    /**
     * @brief 将对象归还到池中以便复用
     * @param obj  要归还的对象指针
     *
     * 内部通过 unordered_set 检测重复释放：若该对象的哈希已在空闲集合中，
     * 则忽略本次 Release，防止同一对象被多次归还导致池状态错误。
     * 归还后对象内存被清零。
     */
    void Release(T* obj)
    {
        if (m_idle_key_set.find(HashKey(obj)) == m_idle_key_set.end())
        {
            m_idle_key_set.insert(HashKey(obj));
            memset(obj, 0, ELEMENT_SIZE);
            m_idles.push(obj);
        }
    }

    // --- 重置 ---

    /**
     * @brief 重置对象池，释放所有内存
     * @param onitem  可选回调，在释放每个 chunk 前对其起始地址调用
     *
     * 调用后池内所有指针失效（包括正在使用的对象），需要重新 Apply。
     */
    void Cleanup(std::function<void(T* item)> onitem = nullptr)
    {
        FreeChunks(onitem);
        std::stack<T*>().swap(m_idles);
        m_idle_key_set.clear();
    }

private:
    /**
     * @brief 将指针地址转换为哈希值（右移 m_shift 位）
     * @param obj  对象指针
     * @return 哈希值，同一 chunk 内相邻对象的哈希值互不相同
     */
    size_t HashKey(void* obj)
    {
        return (size_t)(obj) >> m_shift;
    }

    /**
     * @brief 释放所有已分配的 chunk
     * @param onitem  可选回调，对每个 chunk 的起始地址调用
     */
    void FreeChunks(std::function<void(T* item)> onitem = nullptr)
    {
        while (!m_chunks.empty())
        {
            if (onitem) { onitem((T*)m_chunks.back()); }
            free(m_chunks.back());
            m_chunks.pop_back();
        }
    }

private:
    size_t                      m_chunk_size;       /* 每个 chunk 的对象槽位数 */
    size_t                      m_shift;            /* HashKey 的右移位数 */
    std::vector<void*>          m_chunks;           /* 所有已分配的 chunk 内存块 */
    std::stack<T*>              m_idles;            /* 空闲对象栈（LIFO 复用） */
    std::unordered_set<size_t>  m_idle_key_set;     /* 空闲对象的哈希集合，用于防重复释放 */
};

/**
 * @brief 基于有序数组 + 二分查找的键值表
 *
 * @tparam _TKey  键类型，必须支持 < 和 == 比较，且 sizeof <= sizeof(void*)
 * @tparam _TVal  值类型（以指针形式存储）
 *
 * 内部用 ZmArrayList<_ITEM> 保持按键排序的有序数组，
 * 查找 O(log n)，插入/删除 O(n)（需要移动元素）。
 * 适合小规模且读多写少的场景。
 */
template<typename _TKey, typename _TVal>
class ZmBinaryTable
{
public:
    /**
     * @brief 内部存储的条目结构
     * key 与 val 之间有填充字节，确保 val 字段按指针大小对齐
     */
    typedef struct
    {
        _TKey           key;
        unsigned char   __pad[sizeof(void*) - sizeof(_TKey)];
        _TVal*          val;
    } _ITEM;

private:
    enum { ITEM_SIZE = sizeof(_ITEM) };
    /** Query 的操作类型：查找、插入、删除 */
    enum { ACT_GET = 0, ACT_ADD = 1, ACT_DEL = 2 };

public:
    ZmBinaryTable() {}
    ~ZmBinaryTable() {}

    // --- 容量与状态 ---

    /**
     * @brief 获取当前条目数量
     * @return 条目数
     */
    size_t Count() { return m_items.Count(); }

    // --- 查找 ---

    /**
     * @brief 按键查找对应的值
     * @param key  要查找的键
     * @return 对应的值指针，未找到时返回 NULL
     */
    _TVal* Get(_TKey key)
    {
        _ITEM* item = Query(key);
        return item ? item->val : NULL;
    }

    /**
     * @brief 查找第一个满足条件的值
     * @param fnmatches  匹配谓词
     * @return 第一个匹配的值指针，无匹配时返回 NULL
     */
    _TVal* GetEx(std::function<bool(const _TVal*)> fnmatches)
    {
        _ITEM* it = m_items.Head();
        _ITEM* end = m_items.Head() + m_items.Count();
        while (it < end)
        {
            if (fnmatches(it->val))
            {
                return it->val;
            }
            it++;
        }
        return NULL;
    }

    // --- 插入与删除 ---

    /**
     * @brief 插入或更新一个键值对
     * @param key  键
     * @param val  值指针，为 NULL 时不更新已有条目的 val
     * @return 对应条目的指针
     */
    _ITEM* Put(_TKey key, _TVal* val)
    {
        _ITEM* item = Query(key, ACT_ADD);
        item->key = key;
        if (val)
        {
            item->val = val;
        }
        return item;
    }

    /**
     * @brief 按键删除一个条目
     * @param key  要删除的键
     * @return 被删除条目的临时副本指针（内部静态，下次操作前有效），未找到返回 NULL
     */
    _ITEM* Remove(_TKey key) { return Query(key, ACT_DEL); }

    /**
     * @brief 清空所有条目
     */
    void RemoveAll() { m_items.Clear(); }

    /**
     * @brief 删除满足条件的条目，并将被删除的值收集到 vector
     * @param fnmatches  匹配谓词
     * @param values     输出被删除条目的值指针 vector
     */
    void RemoveEx(std::function<bool(const _TVal*)> fnmatches, std::vector<_TVal*>& values)
    {
        _ITEM* it = m_items.Head();
        _ITEM* end = m_items.Head() + m_items.Count();
        while (it < end)
        {
            if (fnmatches(it->val))
            {
                values.push_back(it->val);
                m_items.Remove(it - m_items.Head());
                end--;
            }
            else
            {
                it++;
            }
        }
    }

    // --- 遍历 ---

    /**
     * @brief 遍历并可选地删除匹配的条目
     * @param fnaction   对每个匹配条目调用的回调，返回 true 表示删除该条目
     * @param fnmatches  匹配谓词，为 nullptr 时匹配所有条目
     *
     * 遍历逻辑：对于每个 fnmatches 为 true（或 fnmatches 为 nullptr）的条目，
     * 调用 fnaction，若 fnaction 返回 true 则删除该条目。
     * 删除后迭代器不递增（因为后续元素已左移填补），仅 end 指针前移。
     */
    void ForEach(std::function<bool(_TVal*)> fnaction, std::function<bool(const _TVal*)> fnmatches = nullptr)
    {
        _ITEM* it = m_items.Head();
        _ITEM* end = m_items.Head() + m_items.Count();
        while (it < end)
        {
            if ((!fnmatches || fnmatches(it->val)) && fnaction(it->val))
            {
                /* 删除当前位置元素，后续元素左移，it 自然指向下一个未处理元素 */
                m_items.Remove(it - m_items.Head());
                end--;
            }
            else
            {
                it++;
            }
        }
    }

    // --- 值收集 ---

    /**
     * @brief 收集所有值到 vector 中
     * @param values  输出的值指针 vector
     */
    void Values(std::vector<_TVal*>& values)
    {
        auto any = [](const _TVal* v) { return true; };
        ValuesEx(any, values);
    }

    /**
     * @brief 收集满足条件的所有值到 vector
     * @param fnmatches  匹配谓词
     * @param values     输出的值指针 vector
     */
    void ValuesEx(std::function<bool(const _TVal*)> fnmatches, std::vector<_TVal*>& values)
    {
        _ITEM* it = m_items.Head();
        _ITEM* end = m_items.Head() + m_items.Count();
        while (it < end)
        {
            if (fnmatches(it->val))
            {
                values.push_back(it->val);
            }
            it++;
        }
    }

    // --- 调试 ---

    /**
     * @brief 打印所有键到 stdout（调试用）
     */
    void DumpKeys()
    {
        _ITEM* it = m_items.Head();
        _ITEM* end = m_items.Head() + m_items.Count();
        while (it < end)
        {
            printf(" %ld,", (long)it->key);
            it++;
        }
    }

private:
    /**
     * @brief 内部统一查询入口，通过二分查找实现查找/插入/删除
     * @param key  目标键
     * @param act  操作类型：ACT_GET（查找）、ACT_ADD（插入）、ACT_DEL（删除）
     * @return ACT_GET: 找到则返回条目指针，未找到返回 NULL；
     *         ACT_ADD: 返回新建或已存在条目的指针；
     *         ACT_DEL: 返回被删除条目的临时副本指针，未找到返回 NULL
     *
     * 二分查找结束后 l 指向第一个 key > 目标 的位置（或末尾），
     * 对于 ACT_ADD，l - head 即为保持有序的正确插入位置。
     */
    _ITEM* Query(_TKey key, int act = ACT_GET)
    {
        if (m_items.Count())
        {
            _ITEM* head = m_items.Head();
            _ITEM* l = head;
            _ITEM* r = head + m_items.Count() - 1;
            while (l <= r)
            {
                _ITEM* m = l + (r - l) / 2;
                if (m->key == key)
                {
                    if (ACT_DEL == act)
                    {
                        memcpy(&m_temp, m, ITEM_SIZE);
                        m_items.Remove(m - head);
                        return &m_temp;
                    }
                    return m;
                }
                else if (m->key < key)
                {
                    l = m + 1;
                }
                else
                {
                    r = m - 1;
                }
            }
            if (ACT_ADD == act)
            {
                return m_items.Insert(l - head);
            }
        }
        return (ACT_ADD == act) ? m_items.Add() : NULL;
    }

private:
    ZmArrayList<_ITEM>  m_items;    /* 有序数组，按键升序排列 */
    _ITEM               m_temp;     /* 删除操作的临时缓冲，保存被删除条目的副本 */
};

#endif /* ZM_UTIL_CONTAINER_HPP */
