#include "container/container.hpp"

#include <fcntl.h>
#include <linux/capability.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "common/registry.hpp"
#include "common/util.hpp"
#include "filesystem/filesystem_manager.hpp"
#include "namespace/namespace_manager.hpp"
#include "process/process_manager.hpp"
#include "resource/resource_manager.hpp"

namespace mc {
namespace {

struct ScopeExit {
    std::function<void()> fn;
    ~ScopeExit() { if (fn) fn(); }
};

// Runs in the workload process just before exec.
void apply_security(CapsMode mode) {
    if (mode == CapsMode::All) return;
    std::vector<int> drop = {CAP_SYS_MODULE, CAP_SYS_RAWIO, CAP_SYS_PTRACE, CAP_SYS_BOOT, CAP_SYS_TIME,
                             CAP_SYSLOG, CAP_MAC_ADMIN, CAP_MAC_OVERRIDE, CAP_AUDIT_CONTROL, CAP_AUDIT_READ,
                             CAP_LINUX_IMMUTABLE, CAP_BLOCK_SUSPEND, CAP_WAKE_ALARM, CAP_SYS_TTY_CONFIG};
    if (mode == CapsMode::Strict)
        for (int c : {CAP_SYS_ADMIN, CAP_NET_ADMIN, CAP_SYS_CHROOT, CAP_SYS_RESOURCE, CAP_MKNOD, CAP_NET_RAW})
            drop.push_back(c);
    for (int c : drop)
        if (::prctl(PR_CAPBSET_DROP, c, 0, 0, 0) != 0 && errno != EINVAL) throw SysError("PR_CAPBSET_DROP");
    if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) throw SysError("PR_SET_NO_NEW_PRIVS");
}

std::int64_t now_unix() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

void write_stats_file(const std::string& path, const ResourceStats& s, int exit_code, long startup_usec) {
    std::ostringstream o;
    o << "exit_code=" << exit_code << "\n"
      << "startup_usec=" << startup_usec << "\n"
      << "memory_peak_bytes=" << s.memory_peak.value_or(0) << "\n"
      << "memory_current_bytes=" << s.memory_current << "\n"
      << "oom_events=" << s.oom_events << "\n"
      << "oom_kill=" << s.oom_kill_events << "\n"
      << "memory_limit_hits=" << s.max_events << "\n"
      << "cpu_usage_usec=" << s.cpu_usage_usec << "\n"
      << "cpu_user_usec=" << s.cpu_user_usec << "\n"
      << "cpu_system_usec=" << s.cpu_system_usec << "\n"
      << "nr_periods=" << s.nr_periods << "\n"
      << "nr_throttled=" << s.nr_throttled << "\n"
      << "throttled_usec=" << s.throttled_usec << "\n";
    write_text_file(path, o.str());
}

}  // namespace

void Container::validate() {
    if (::geteuid() != 0) throw std::runtime_error("minicontainer needs root privileges (run with sudo)");
    if (cfg_.name.empty()) cfg_.name = "mc-" + random_hex(3);
    if (!valid_name(cfg_.name)) throw std::invalid_argument("invalid container name '" + cfg_.name + "' (use letters, digits, '_', '-', '.')");
    if (cfg_.hostname.empty()) cfg_.hostname = cfg_.name.substr(0, 63);
    if (cfg_.hostname.size() > 63) throw std::invalid_argument("hostname longer than 63 characters");
    for (unsigned char c : cfg_.hostname)
        if (!(std::isalnum(c) || c == '-' || c == '.')) throw std::invalid_argument("invalid hostname '" + cfg_.hostname + "'");
    if (cfg_.command.empty()) throw std::invalid_argument("no command given (use: run [options] -- <command> [args...])");
    if (cfg_.grace_seconds < 0) throw std::invalid_argument("grace period must be >= 0");

    cfg_.rootfs = FilesystemManager::validate_rootfs(cfg_.rootfs);
    FilesystemManager::validate_command(cfg_.rootfs, cfg_.command[0]);
    ResourceManager::validate_limits({cfg_.memory_bytes, cfg_.cpu_percent, cfg_.pids_max});

    if (auto st = Registry::load(cfg_.name)) {
        if (Registry::is_running(*st)) throw std::runtime_error("container '" + cfg_.name + "' is already running");
        Registry::remove(cfg_.name);  // stale record from a crashed runner
    }
    ResourceManager::remove_cgroup(ResourceManager::cgroup_path(cfg_.name));  // stale cgroup, if any
}

int Container::run() {
    validate();
    return cfg_.detach ? run_detached() : run_foreground();
}

int Container::run_detached() {
    mkdir_p(Registry::dir());
    std::cout.flush();
    pid_t d = ::fork();
    if (d < 0) throw SysError("fork");
    if (d > 0) {
        for (int i = 0; i < 100; ++i) {  // wait up to ~5s for the daemon to register or die
            if (auto st = Registry::load(cfg_.name))
                if (Registry::is_running(*st)) {
                    std::cout << cfg_.name << std::endl;
                    return 0;
                }
            int status;
            if (::waitpid(d, &status, WNOHANG) == d) {
                std::cerr << "minicontainer: container failed to start; see " << Registry::log_path(cfg_.name) << std::endl;
                return 125;
            }
            ::usleep(50000);
        }
        std::cerr << "minicontainer: timed out waiting for '" << cfg_.name << "' to start" << std::endl;
        return 125;
    }
    // ---- daemon (runner) ----
    ::setsid();
    int log = ::open(Registry::log_path(cfg_.name).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    int nul = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (log >= 0) { ::dup2(log, 1); ::dup2(log, 2); }
    if (nul >= 0) ::dup2(nul, 0);
    int code = 125;
    try {
        code = run_foreground();
    } catch (const std::exception& e) {
        std::cerr << "minicontainer: error: " << e.what() << std::endl;
    }
    ::_exit(code);
}

int Container::run_foreground() {
    const ResourceLimits limits{cfg_.memory_bytes, cfg_.cpu_percent, cfg_.pids_max};
    ResourceManager::prepare_hierarchy(limits);

    ResourceManager rm(cfg_.name);
    ProcessManager pm;
    bool registered = false;
    // Single cleanup path: runs on success, on every error, and on exceptions (FR-12).
    ScopeExit cleanup{[&] {
        pm.kill_and_reap();
        rm.destroy();
        if (registered) Registry::remove(cfg_.name);
    }};

    const auto t0 = std::chrono::steady_clock::now();
    rm.create(limits);

    ChildContext ctx;
    ctx.argv = cfg_.command;
    const char* term = std::getenv("TERM");
    ctx.envp = {"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "HOME=/root",
                std::string("TERM=") + (term ? term : "xterm"), "HOSTNAME=" + cfg_.hostname, "container=minicontainer"};
    const std::string hostname = cfg_.hostname, rootfs = cfg_.rootfs;
    const bool net = cfg_.isolate_net;
    const CapsMode caps = cfg_.caps;
    ctx.prepare = [hostname, rootfs, net] {
        NamespaceManager::set_hostname(hostname);
        FilesystemManager::enter_root(rootfs);
        if (net) NamespaceManager::bring_up_loopback();
    };
    ctx.pre_exec = [caps] { apply_security(caps); };
    ctx.use_init = !cfg_.no_init;

    pm.set_grace_period(cfg_.grace_seconds);
    const pid_t pid = pm.spawn(NamespaceManager::clone_flags(cfg_), ctx);

    // Race-free ordering (PRD 4.1): child is blocked on the pipe; join the cgroup FIRST, then release.
    try {
        rm.add_process(pid);
    } catch (...) {
        pm.cancel();
        throw;
    }

    ContainerState st;
    st.name = cfg_.name;
    st.pid = pid;
    st.runner_pid = ::getpid();
    st.cgroup = rm.path();
    st.rootfs = cfg_.rootfs;
    st.hostname = cfg_.hostname;
    st.command = join(cfg_.command, " ");
    for (auto& ch : st.command) if (ch == '\n') ch = ' ';
    st.memory_bytes = cfg_.memory_bytes;
    st.cpu_percent = cfg_.cpu_percent;
    st.pids_max = cfg_.pids_max;
    st.grace = cfg_.grace_seconds;
    st.isolate_net = cfg_.isolate_net;
    st.detached = cfg_.detach;
    st.started = now_unix();
    Registry::save(st);
    registered = true;

    pm.release();
    const std::string child_err = pm.read_child_error();  // returns once the workload exec'd (or failed)
    const long startup_usec =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();

    int code = pm.wait();
    if (!child_err.empty()) {
        auto nl = child_err.find('\n');
        const int err_code = std::atoi(child_err.substr(0, nl).c_str());
        std::cerr << "minicontainer: error: " << (nl == std::string::npos ? child_err : child_err.substr(nl + 1)) << std::endl;
        return err_code ? err_code : 125;
    }

    const ResourceStats stats = rm.read_stats();  // must be read before the cgroup is removed
    if (!cfg_.stats_file.empty()) write_stats_file(cfg_.stats_file, stats, code, startup_usec);
    if (!cfg_.quiet) {
        std::cerr << "[minicontainer] '" << cfg_.name << "' exited with status " << code << " (startup "
                  << startup_usec / 1000.0 << " ms)\n";
        std::istringstream in(ResourceManager::format_summary(stats));
        std::string line;
        while (std::getline(in, line)) std::cerr << "[minicontainer] " << line << "\n";
        std::cerr.flush();
    }
    return code;
}

}  // namespace mc
