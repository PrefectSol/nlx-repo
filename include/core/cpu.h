#ifndef NLX_CORE_CPU_H
#define NLX_CORE_CPU_H

#include <initializer_list>
#include <sched.h>
#include <pthread.h>

namespace nlx::cpu
{

[[nodiscard]] bool set_affinity(pthread_t thread, std::initializer_list<int> cpus) noexcept;
[[nodiscard]] bool set_scheduling(pthread_t thread, int policy, int priority) noexcept;
[[nodiscard]] bool set_name(pthread_t thread, const char *name) noexcept;

[[nodiscard]] pthread_t this_native_handle() noexcept;

} // namespace nlx::cpu
#endif // !NLX_CORE_CPU_H

