#include "resource/resource_manager.hpp"

#include <signal.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <stdexcept>

#include "common/util.hpp"

#ifndef CGROUP2_SUPER_MAGIC
#define CGROUP2_SUPER_MAGIC 0x63677270
#endif

namespace mc {
namespace {

bool has_word(const std::string& haystack, const std::string& word) {
    std::istringstream in(haystack);
    std::string w;
    while (in >> w)
        if (w == word) return true;
    return false;
}

std::string root_path(const std::string& rel) { return ResourceManager::cgroup_root() + "/" + rel; }
std::string parent_path() { return root_path(ResourceManager::kParentName); }

std::string read_trim(const std::string& path) {
    auto t = read_file(path);
    return t ? trim(*t) : std::string();
}

std::optional<std::uint64_t> read_u64(const std::string& path) {
    auto t = read_file(path);
    if (!t) return std::nullopt;
    try {
        return std::stoull(trim(*t));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

std::string ResourceManager::cgroup_root() {
    const char* env = std::getenv("MC_CGROUP_ROOT");
    return (env && *env) ? env : "/sys/fs/cgroup";
}

ResourceManager::ResourceManager(std::string name) : name_(std::move(name)), path_(cgroup_path(name_)) {}

ResourceManager::~ResourceManager() { destroy(); }

std::string ResourceManager::cgroup_path(const std::string& name) { return parent_path() + "/" + name; }

std::string ResourceManager::cpu_max_value(int percent) {
    // 50% of one CPU = 50000us of CPU time every 100000us period.
    return std::to_string(static_cast<long long>(percent) * kCpuPeriodUsec / 100) + " " + std::to_string(kCpuPeriodUsec);
}

void ResourceManager::validate_limits(const ResourceLimits& l) {
    if (l.memory_bytes != 0 && l.memory_bytes < (1ULL << 20))
        throw std::invalid_argument("memory limit must be at least 1 MiB");
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    if (l.cpu_percent < 0 || l.cpu_percent > 100 * ncpu)
        throw std::invalid_argument("cpu limit must be between 1 and " + std::to_string(100 * ncpu) +
                                    " (percent of one CPU; this machine has " + std::to_string(ncpu) + " CPUs)");
    if (l.pids_max < 0) throw std::invalid_argument("pids limit must be positive");
}

void ResourceManager::prepare_hierarchy(const ResourceLimits& limits) {
    struct statfs sfs;
    if (statfs(cgroup_root().c_str(), &sfs) != 0 || sfs.f_type != CGROUP2_SUPER_MAGIC)
        throw std::runtime_error("cgroups v2 is not mounted at " + cgroup_root() +
                                 " (the unified hierarchy is required; on Ubuntu 22.04+ it is the default)");

    auto available = read_file(root_path("cgroup.controllers"));
    if (!available) throw SysError("read " + root_path("cgroup.controllers"));
    for (const char* c : {"memory", "cpu"})
        if (!has_word(*available, c))
            throw std::runtime_error(std::string("cgroup controller '") + c + "' is not available (have: " + trim(*available) + ")");
    if (limits.pids_max > 0 && !has_word(*available, "pids"))
        throw std::runtime_error("cgroup controller 'pids' is not available");

    if (::mkdir(parent_path().c_str(), 0755) != 0 && errno != EEXIST) throw SysError("mkdir " + parent_path());

    // Delegate controllers: root -> /minicontainer -> /minicontainer/<name>.
    // Writing "+x" is idempotent; failures are tolerated here and verified below.
    for (const char* c : {"memory", "cpu", "pids"}) {
        if (!has_word(*available, c)) continue;
        try { write_control_file(root_path("cgroup.subtree_control"), std::string("+") + c); } catch (const std::exception&) {}
    }
    for (const char* c : {"memory", "cpu", "pids"}) {
        if (!has_word(*available, c)) continue;
        try { write_control_file(parent_path() + "/cgroup.subtree_control", std::string("+") + c); } catch (const std::exception&) {}
    }
    const std::string enabled = read_trim(parent_path() + "/cgroup.subtree_control");
    for (const char* c : {"memory", "cpu"})
        if (!has_word(enabled, c))
            throw std::runtime_error(std::string("could not enable the '") + c + "' controller for " + parent_path() +
                                     ". Is this a nested cgroup namespace (e.g. inside Docker) or a cgroup with processes? "
                                     "Use a dedicated Ubuntu VM.");
    if (limits.pids_max > 0 && !has_word(enabled, "pids"))
        throw std::runtime_error("could not enable the 'pids' controller");
}

void ResourceManager::create(const ResourceLimits& l) {
    if (::mkdir(path_.c_str(), 0755) != 0) throw SysError("mkdir " + path_);
    created_ = true;
    if (l.memory_bytes) {
        write_control_file(path_ + "/memory.max", std::to_string(l.memory_bytes));
        // Without this the kernel could satisfy the limit by swapping instead of enforcing it.
        try { write_control_file(path_ + "/memory.swap.max", "0"); } catch (const std::exception&) {}
    }
    if (l.cpu_percent) write_control_file(path_ + "/cpu.max", cpu_max_value(l.cpu_percent));
    if (l.pids_max) write_control_file(path_ + "/pids.max", std::to_string(l.pids_max));
}

void ResourceManager::add_process(pid_t pid) { write_control_file(path_ + "/cgroup.procs", std::to_string(pid)); }

ResourceStats ResourceManager::read_stats_at(const std::string& dir) {
    ResourceStats s;
    s.memory_current = read_u64(dir + "/memory.current").value_or(0);
    s.memory_peak = read_u64(dir + "/memory.peak");
    s.memory_max = read_trim(dir + "/memory.max");
    if (auto ev = read_file(dir + "/memory.events")) {
        auto kv = parse_kv(*ev);
        s.oom_events = kv["oom"];
        s.oom_kill_events = kv["oom_kill"];
        s.max_events = kv["max"];
    }
    s.cpu_max = read_trim(dir + "/cpu.max");
    if (auto cs = read_file(dir + "/cpu.stat")) {
        auto kv = parse_kv(*cs);
        s.cpu_usage_usec = kv["usage_usec"];
        s.cpu_user_usec = kv["user_usec"];
        s.cpu_system_usec = kv["system_usec"];
        s.nr_periods = kv["nr_periods"];
        s.nr_throttled = kv["nr_throttled"];
        s.throttled_usec = kv["throttled_usec"];
    }
    s.pids_current = read_u64(dir + "/pids.current");
    s.pids_max = read_trim(dir + "/pids.max");
    return s;
}

std::string ResourceManager::format_summary(const ResourceStats& s) {
    auto bytes_or_unlimited = [](const std::string& v) -> std::string {
        if (v.empty() || v == "max") return "unlimited";
        try { return human_bytes(std::stoull(v)); } catch (...) { return v; }
    };
    char buf[512];
    std::string out;
    std::snprintf(buf, sizeof buf, "memory: peak=%s limit=%s oom_kill=%llu limit_hits=%llu\n",
                  s.memory_peak ? human_bytes(*s.memory_peak).c_str() : "n/a", bytes_or_unlimited(s.memory_max).c_str(),
                  static_cast<unsigned long long>(s.oom_kill_events), static_cast<unsigned long long>(s.max_events));
    out += buf;
    std::snprintf(buf, sizeof buf, "cpu:    usage=%.3fs (user %.3fs, system %.3fs) throttled=%llu/%llu periods (%.3fs) cpu.max=\"%s\"",
                  s.cpu_usage_usec / 1e6, s.cpu_user_usec / 1e6, s.cpu_system_usec / 1e6,
                  static_cast<unsigned long long>(s.nr_throttled), static_cast<unsigned long long>(s.nr_periods),
                  s.throttled_usec / 1e6, s.cpu_max.c_str());
    out += buf;
    return out;
}

void ResourceManager::remove_cgroup(const std::string& dir) noexcept {
    try {
        // Kill whatever is left (cgroup.kill: kernel >= 5.14, otherwise walk cgroup.procs).
        try {
            write_control_file(dir + "/cgroup.kill", "1");
        } catch (const std::exception&) {
            if (auto procs = read_file(dir + "/cgroup.procs")) {
                std::istringstream in(*procs);
                long p;
                while (in >> p)
                    if (p > 1) ::kill(static_cast<pid_t>(p), SIGKILL);
            }
        }
        // rmdir fails with EBUSY until the dying processes are fully gone.
        for (int i = 0; i < 100; ++i) {
            if (::rmdir(dir.c_str()) == 0 || errno == ENOENT) return;
            if (errno != EBUSY) return;
            ::usleep(20000);
        }
    } catch (...) {
    }
}

void ResourceManager::destroy() noexcept {
    if (!created_) return;
    remove_cgroup(path_);
    created_ = false;
}

}  // namespace mc
