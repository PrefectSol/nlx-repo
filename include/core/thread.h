#ifndef NLX_CORE_THREAD_H
#define NLX_CORE_THREAD_H

#include <cstddef>
#include <initializer_list>

#include <pthread.h>
#include <sched.h>

namespace nlx::thread
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
    void start(T &object, const char *name, size_t stack_size, int policy, int priority, std::initializer_list<int> cpus);

    [[nodiscard]] bool joinable() const noexcept;
    void join() noexcept;

private:
    static constexpr size_t kNameCapacity = 16;

    pthread_t m_handle;
    bool m_joinable;

    void (*m_routine)(void *) noexcept;
    void *m_object;

    char m_name[kNameCapacity];

    void create(void (*routine)(void *) noexcept, void *object, const char *name, size_t stack_size, int policy, int priority, std::initializer_list<int> cpus);

    static void *run(void *context) noexcept;

    template<typename T>
    static void invoke(void *object) noexcept;
};

[[nodiscard]] bool set_affinity(pthread_t thread, std::initializer_list<int> cpus) noexcept;
[[nodiscard]] bool set_scheduling(pthread_t thread, int policy, int priority) noexcept;
[[nodiscard]] bool set_name(pthread_t thread, const char *name) noexcept;

[[nodiscard]] pthread_t this_native_handle() noexcept;

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

} // namespace nlx::thread::detail
} // namespace nlx::thread

namespace nlx::thread
{

template<typename T>
void Thread::start(T &object, const char *name, size_t stack_size, int policy, int priority, std::initializer_list<int> cpus)
{
    create(invoke<T>, &object, name, stack_size, policy, priority, cpus);
}

template<typename T>
void Thread::invoke(void *object) noexcept
{
    static_cast<T *>(object)->start();
}

} // namespace nlx::thread
#endif // !NLX_CORE_THREAD_H
