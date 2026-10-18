#include "core/cpu.h"
#include "core/system_error.h"

#include <sched.h>
#include <stdexcept>
#include <format>

namespace nlx::cpu
{

void set_affinity(pthread_t thread, std::initializer_list<int> cpus)
{
    cpu_set_t cpuset{};
    CPU_ZERO(&cpuset);

    for (int cpu : cpus)
    {
        if (cpu < 0 || cpu >= CPU_SETSIZE) [[unlikely]]
        {
            throw std::out_of_range(std::format("cpu {} out of range [0, {})", cpu, CPU_SETSIZE));
        }

        CPU_SET(cpu, &cpuset);
    }

    const int result = ::pthread_setaffinity_np(thread, sizeof(cpuset), &cpuset);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_setaffinity_np failed");
    }
}

void set_scheduling(pthread_t thread, int policy, int priority)
{
    sched_param param{};
    param.sched_priority = priority;

    const int result = ::pthread_setschedparam(thread, policy, &param);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_setschedparam failed");
    }
}

void set_name(pthread_t thread, const char *name)
{
    if (name == nullptr) [[unlikely]]
    {
        throw std::invalid_argument("thread name cannot be nullptr");
    }

    const int result = ::pthread_setname_np(thread, name);
    if (result != 0) [[unlikely]]
    {
        throw SystemError(result, "pthread_setname_np failed");
    }
}

} // namespace nlx::cpu
