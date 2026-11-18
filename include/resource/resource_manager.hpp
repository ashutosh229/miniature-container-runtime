#pragma once
// Resource Manager: owns one cgroup-v2 directory per container.
#include <sys/types.h>

#include <cstdint>
#include <optional>
#include <string>

namespace mc {

struct ResourceLimits {
    std::uint64_t memory_bytes = 0;  // 0 = unlimited
    int cpu_percent = 0;             // 0 = unlimited, 100 = one full CPU
    int pids_max = 0;                // 0 = unlimited
};

struct ResourceStats {
    std::uint64_t memory_current = 0;
    std::optional<std::uint64_t> memory_peak;  // memory.peak (kernel >= 5.19)
    std::string memory_max;                    // raw "max" or bytes
    std::uint64_t oom_events = 0, oom_kill_events = 0, max_events = 0;
    std::string cpu_max;  // raw "quota period" or "max period"
    std::uint64_t cpu_usage_usec = 0, cpu_user_usec = 0, cpu_system_usec = 0;
    std::uint64_t nr_periods = 0, nr_throttled = 0, throttled_usec = 0;
    std::optional<std::uint64_t> pids_current;
    std::string pids_max;
};

class ResourceManager {
public:
    // /sys/fs/cgroup, or $MC_CGROUP_ROOT (for testing against a cgroup2 mounted elsewhere)
    static std::string cgroup_root();
    static constexpr const char* kParentName = "minicontainer";  // /sys/fs/cgroup/minicontainer/<name>
    static constexpr int kCpuPeriodUsec = 100000;

    explicit ResourceManager(std::string name);
    ~ResourceManager();
    ResourceManager(const ResourceManager&) = delete;
    ResourceManager& operator=(const ResourceManager&) = delete;

    // ---- static helpers (usable without an instance) ----
    static void validate_limits(const ResourceLimits& limits);        // sanity-check numbers
    static void prepare_hierarchy(const ResourceLimits& limits);      // cgroup v2 + controllers + parent dir
    static std::string cpu_max_value(int percent);                    // 50 -> "50000 100000"
    static std::string cgroup_path(const std::string& name);
    static ResourceStats read_stats_at(const std::string& cgroup_dir);
    static std::string format_summary(const ResourceStats& s);        // multi-line, human readable
    static void remove_cgroup(const std::string& cgroup_dir) noexcept;  // kill members + rmdir

    // ---- per-container ----
    void create(const ResourceLimits& limits);
    void add_process(pid_t host_pid);  // writes to cgroup.procs
    ResourceStats read_stats() const { return read_stats_at(path_); }
    void destroy() noexcept;           // idempotent
    const std::string& path() const { return path_; }

private:
    std::string name_;
    std::string path_;
    bool created_ = false;
};

}  // namespace mc
