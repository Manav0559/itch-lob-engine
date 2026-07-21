#pragma once

// Cross-platform "pin the calling thread to a CPU core" helper for
// pipeline::run_pipeline's parser/book-builder threads (see threaded_replay.hpp).
//
// This exists because the original threaded-pipeline benchmark
// (bench/THREADED_PIPELINE_FINDINGS.md) measured parsing-vs-book-building
// decoupled onto two threads WITHOUT pinning either to a specific core — on a
// busy/shared machine, the OS scheduler is free to migrate a thread between
// cores mid-run, which destroys the L1/L2 cache locality decoupling is
// supposed to let you keep. A "threading doesn't help" verdict reached without
// controlling for that variable is measuring thread migration noise as much
// as it's measuring the design. This header is what lets both the benchmark
// and the real replay_threaded binary actually control that variable.
#if defined(__linux__)
#include <pthread.h>
#include <sched.h>

namespace pipeline {

// Hard pin: on Linux, pthread_setaffinity_np actually binds the calling
// thread to the given core - the scheduler will not migrate it elsewhere.
inline bool pin_this_thread_to_core(int core) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core, &cpuset);
    return pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset) == 0;
}

// True hard pinning is available on this platform - a benchmark comparing
// pinned vs. unpinned here is measuring what it claims to measure.
inline constexpr bool kHasHardAffinity = true;

}  // namespace pipeline

#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/thread_policy.h>
#include <pthread.h>

namespace pipeline {

// macOS has no public API for hard core pinning. THREAD_AFFINITY_POLICY is
// only a hint: threads sharing the same affinity tag are preferentially
// scheduled to share an L2 cache, not bound to a specific core - and on
// Apple Silicon's heterogeneous performance/efficiency-core design, the
// kernel scheduler already makes most placement decisions itself, so this
// hint carries less weight than the same call would on Intel. A `true`
// return here means "the OS accepted the affinity tag," not "the thread is
// now pinned to a core" - stated plainly rather than implied by a
// same-named function that behaves differently per platform.
inline bool pin_this_thread_to_core(int core) {
    thread_affinity_policy_data_t policy = {core + 1};  // tag 0 is reserved/default
    const thread_port_t thread = pthread_mach_thread_np(pthread_self());
    return thread_policy_set(thread, THREAD_AFFINITY_POLICY,
                              reinterpret_cast<thread_policy_t>(&policy),
                              THREAD_AFFINITY_POLICY_COUNT) == KERN_SUCCESS;
}

// Only a scheduling hint on this platform, not a hard pin - see the
// function comment above. A pinned-vs-unpinned comparison run here is
// suggestive, not conclusive; bench/bench_threaded_main.cpp's pinned path
// says so explicitly rather than reporting a false-confidence verdict.
inline constexpr bool kHasHardAffinity = false;

}  // namespace pipeline

#else
namespace pipeline {

// No affinity support wired up for this platform - always a no-op.
inline bool pin_this_thread_to_core(int /*core*/) { return false; }
inline constexpr bool kHasHardAffinity = false;

}  // namespace pipeline
#endif
