#pragma once
// How the command-line tools spread a corpus over threads (niffer-tool, niffer-yaml): NIFFER_TOOL_THREADS workers (1-256), else
// half the machine, so a gate never takes every core. Tools only; the library itself starts no thread.

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <thread>
#include <vector>

inline unsigned ToolThreads() {
    if (const char* e = std::getenv("NIFFER_TOOL_THREADS")) {
        char*      end = nullptr;
        const long v   = std::strtol(e, &end, 10);
        if (end != e && *end == '\0' && v >= 1 && v <= 256) return static_cast<unsigned>(v);
    }
    return std::max(1u, std::thread::hardware_concurrency() / 2);
}

// out[i] = fn(in[i]) on ToolThreads() workers, each taking the next index.
template <class In, class Out, class Fn>
void ParallelMap(const std::vector<In>& in, std::vector<Out>& out, Fn fn) {
    std::atomic<size_t> next{ 0 };
    auto work = [&] {
        for (size_t i = next.fetch_add(1); i < in.size(); i = next.fetch_add(1)) out[i] = fn(in[i]);
    };
    const size_t n = std::min<size_t>(ToolThreads(), std::max<size_t>(1, in.size()));
    std::vector<std::thread> pool;
    for (size_t t = 1; t < n; ++t) pool.emplace_back(work);
    work();
    for (auto& t : pool) t.join();
}
