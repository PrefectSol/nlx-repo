#include "core/thread.h"
#include "core/fatal.h"
#include "core/string.h"
#include "core/system_error.h"

#include <cstring>
#include <stdexcept>
#include <string_view>

namespace nlx::thread
{

namespace
{

bool make_cpuset(std::initializer_list<int> cpus, cpu_set_t &cpuset) noexcept
{
    CPU_ZERO(&cpuset);

    for (int cpu : cpus)
    {
        if (cpu < 0 || cpu >= CPU_SETSIZE) [[unlikely]]
        {
            return false;
        }

        CPU_SET(cpu, &cpuset);
    }

    return true;
}

} // namespace

Thread::Thread() noexcept :
    m_handle(),
    m_joinable(false),
    m_routine(nullptr),
    m_object(nullptr),
    m_name()
{

}

Thread::~Thread() noexcept
{
    if (m_joinable) [[unlikely]]
    {
        NLX_FATAL("nlx::thread::Thread::~Thread() thread is still joinable");
    }
}

bool Thread::joinable() const noexcept
{
    return m_joinable;
}

void Thread::join() noexcept
{
    if (!m_joinable) [[unlikely]]
    {
        NLX_FATAL("nlx::thread::Thread::join() thread is not joinable");
    }

    const int result = ::pthread_join(m_handle, nullptr);
    if (result != 0) [[unlikely]]
    {
        NLX_FATAL("nlx::thread::Thread::join() pthread_join failed");
    }

    m_joinable = false;
}

void Thread::create(void (*routine)(void *) noexcept, void *object, const char *name, size_t stack_size, int policy, int priority, std::initializer_list<int> cpus)
{
    if (m_joinable) [[unlikely]]
    {
        throw std::logic_error("thread is already started");
    }

    const bool named = name != nullptr && string::safe_strcpy(m_name, std::string_view(name, ::strnlen(name, kNameCapacity)));
    if (!named) [[unlikely]]
    {
        throw std::invalid_argument("invalid thread name");
    }

    cpu_set_t cpuset{};
    if (!make_cpuset(cpus, cpuset)) [[unlikely]]
    {
        throw std::invalid_argument("invalid thread cpu");
    }

    m_routine = routine;
    m_object = object;

    detail::ThreadAttributes attributes;

    int result = ::pthread_attr_setstacksize(attributes.get(), stack_size);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_attr_setstacksize failed");
    }

    result = ::pthread_attr_setinheritsched(attributes.get(), PTHREAD_EXPLICIT_SCHED);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_attr_setinheritsched failed");
    }

    result = ::pthread_attr_setschedpolicy(attributes.get(), policy);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_attr_setschedpolicy failed");
    }

    sched_param param{};
    param.sched_priority = priority;

    result = ::pthread_attr_setschedparam(attributes.get(), &param);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_attr_setschedparam failed");
    }

    if (cpus.size() != 0)
    {
        result = ::pthread_attr_setaffinity_np(attributes.get(), sizeof(cpuset), &cpuset);
        if (result != 0) [[unlikely]]
        {
            throw SystemError(result, "pthread_attr_setaffinity_np failed");
        }
    }

    result = ::pthread_create(&m_handle, attributes.get(), run, this);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_create failed");
    }

    m_joinable = true;
}

void *Thread::run(void *context) noexcept
{
    Thread &thread = *static_cast<Thread *>(context);

    if (!set_name(this_native_handle(), thread.m_name)) [[unlikely]]
    {
        NLX_FATAL("nlx::thread::Thread::run() set_name failed");
    }

    thread.m_routine(thread.m_object);

    return nullptr;
}

bool set_affinity(pthread_t thread, std::initializer_list<int> cpus) noexcept
{
    if (cpus.size() == 0)
    {
        return true;
    }

    cpu_set_t cpuset{};
    if (!make_cpuset(cpus, cpuset)) [[unlikely]]
    {
        return false;
    }

    return ::pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset) == 0;
}

bool set_scheduling(pthread_t thread, int policy, int priority) noexcept
{
    sched_param param{};
    param.sched_priority = priority;

    return ::pthread_setschedparam(thread, policy, &param) == 0;
}

bool set_name(pthread_t thread, const char *name) noexcept
{
    return name != nullptr && ::pthread_setname_np(thread, name) == 0;
}

pthread_t this_native_handle() noexcept
{
    return ::pthread_self();
}

namespace detail
{

ThreadAttributes::ThreadAttributes()
{
    const int result = ::pthread_attr_init(&m_attributes);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_attr_init failed");
    }
}

ThreadAttributes::~ThreadAttributes() noexcept
{
    ::pthread_attr_destroy(&m_attributes);
}

pthread_attr_t *ThreadAttributes::get() noexcept
{
    return &m_attributes;
}

} // namespace nlx::thread::detail
} // namespace nlx::thread
