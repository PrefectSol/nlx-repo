#ifndef NLX_CORE_THREAD_H
#define NLX_CORE_THREAD_H

#include <atomic>
#include <cstddef>
#include <pthread.h>
#include <sched.h>

namespace nlx
{

class Thread
{
public:
    Thread() noexcept;
    ~Thread() noexcept;

    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;
    Thread(Thread &&) = delete;
    Thread &operator=(Thread &&) = delete;

    template<typename T>
    void start(T &object, const char *name, size_t stack_size, int policy, int priority);

    [[nodiscard]] bool joinable() const noexcept;
    void join() noexcept;

private:
    static constexpr size_t kNameCapacity = 16;
    static constexpr int kPending = -1;

    pthread_t m_handle;
    bool m_joinable;

    void (*m_routine)(void *) noexcept;
    void *m_object;

    char m_name[kNameCapacity];
    int m_policy;
    int m_priority;

    std::atomic<int> m_status;

    void create(void (*routine)(void *) noexcept, void *object, const char *name, size_t stack_size, int policy, int priority);

    static void *run(void *context) noexcept;

    template<typename T>
    static void invoke(void *object) noexcept;
};

namespace detail
{

class ThreadAttributes
{
public:
    ThreadAttributes();
    ~ThreadAttributes() noexcept;

    ThreadAttributes(const ThreadAttributes &) = delete;
    ThreadAttributes &operator=(const ThreadAttributes &) = delete;
    ThreadAttributes(ThreadAttributes &&) = delete;
    ThreadAttributes &operator=(ThreadAttributes &&) = delete;

    [[nodiscard]] pthread_attr_t *get() noexcept;

private:
    pthread_attr_t m_attributes;
};

} // namespace nlx::detail
} // namespace nlx

namespace nlx
{

template<typename T>
void Thread::start(T &object, const char *name, size_t stack_size, int policy, int priority)
{
    create(invoke<T>, &object, name, stack_size, policy, priority);
}

template<typename T>
void Thread::invoke(void *object) noexcept
{
    static_cast<T *>(object)->start();
}

} // namespace nlx
#endif // !NLX_CORE_THREAD_H
