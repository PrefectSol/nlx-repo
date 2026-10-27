#include "core/thread.h"
#include "core/cpu.h"
#include "core/fatal.h"
#include "core/string.h"
#include "core/system_error.h"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace nlx
{

Thread::Thread() noexcept :
    m_handle(),
    m_joinable(false),
    m_routine(nullptr),
    m_object(nullptr),
    m_name(),
    m_policy(SCHED_OTHER),
    m_priority(0),
    m_status(kPending)
{

}

Thread::~Thread() noexcept
{
    if (m_joinable) [[unlikely]]
    {
        NLX_FATAL("nlx::Thread::~Thread() thread is still joinable");
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
        NLX_FATAL("nlx::Thread::join() thread is not joinable");
    }

    const int result = ::pthread_join(m_handle, nullptr);
    if (result != 0) [[unlikely]]
    {
        NLX_FATAL("nlx::Thread::join() pthread_join failed");
    }

    m_joinable = false;
}

void Thread::create(void (*routine)(void *) noexcept, void *object, const char *name, size_t stack_size, int policy, int priority)
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

    m_routine = routine;
    m_object = object;
    m_policy = policy;
    m_priority = priority;
    m_status.store(kPending, std::memory_order_relaxed);

    detail::ThreadAttributes attributes;

    int result = ::pthread_attr_setstacksize(attributes.get(), stack_size);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_attr_setstacksize failed");
    }

    result = ::pthread_create(&m_handle, attributes.get(), run, this);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_create failed");
    }

    m_status.wait(kPending, std::memory_order_acquire);

    result = m_status.load(std::memory_order_acquire);
    if (result != 0) [[unlikely]]
    {
        ::pthread_join(m_handle, nullptr);
        throw SystemError(result, "thread setup failed");
    }

    m_joinable = true;
}

void *Thread::run(void *context) noexcept
{
    Thread &thread = *static_cast<Thread *>(context);
    const pthread_t handle = cpu::this_native_handle();

    bool configured = cpu::set_name(handle, thread.m_name);
    configured = configured && cpu::set_scheduling(handle, thread.m_policy, thread.m_priority);

    thread.m_status.store(configured ? 0 : errno, std::memory_order_release);
    thread.m_status.notify_one();

    if (!configured)
    {
        return nullptr;
    }

    thread.m_routine(thread.m_object);

    return nullptr;
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

} // namespace nlx::detail
} // namespace nlx
