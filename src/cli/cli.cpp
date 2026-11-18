#include "cli/cli.hpp"

#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include "common/registry.hpp"
#include "common/util.hpp"
#include "container/container.hpp"
#include "namespace/namespace_manager.hpp"
#include "resource/resource_manager.hpp"

namespace mc {
namespace {

class UsageError : public std::runtime_error {
    using std::runtime_error::runtime_error;
};

const char* kVersion = "minicontainer 1.0.0";

const char* kUsage = R"(MiniContainer - a lightweight Linux container runtime (namespaces + cgroups v2)

USAGE
  minicontainer run [options] -- <command> [args...]
  minicontainer ps
  minicontainer inspect <name>
  minicontainer stop <name>
  minicontainer logs <name>
  minicontainer clean
  minicontainer --help | --version

RUN OPTIONS
  --rootfs DIR        root filesystem directory (required; see scripts/setup_rootfs.sh)
  --name NAME         container name (default: mc-<random>)
  --hostname NAME     hostname inside the container (default: the name)
  --memory SIZE       memory limit, e.g. 100M, 1G, 512K          (cgroup memory.max)
  --cpu PERCENT       CPU quota, 50 = half of one CPU, 200 = two (cgroup cpu.max)
  --pids N            maximum number of processes                (cgroup pids.max)
  --isolate-net       also create a network namespace (loopback only)
  --caps MODE         default | strict | all  (capability bounding set; default drops
                      module/rawio/ptrace/boot/time/..., strict also drops SYS_ADMIN, NET_ADMIN, ...)
  --grace SECONDS     SIGTERM -> SIGKILL grace period on stop/Ctrl+C (default 5)
  --no-init           make the command itself PID 1 (default: a tiny init is PID 1 and
                      reaps zombies / forwards signals; the command is then PID 2)
  -d, --detach        run in the background; use ps / inspect / logs / stop
  -q, --quiet         do not print the resource summary on exit
  --stats-file FILE   write machine-readable statistics (key=value) on exit

EXAMPLES
  sudo ./minicontainer run --name demo --rootfs ./rootfs --memory 100M --cpu 50 \
       --hostname isolated -- /bin/sh
  sudo ./minicontainer run -d --name web --rootfs ./rootfs -- /bin/sleep 600
  sudo ./minicontainer inspect web && sudo ./minicontainer stop web

Exit status of `run` is the command's exit status (128+N if killed by signal N);
125 = runtime error, 126 = cannot execute, 127 = command not found, 2 = usage error.
)";

bool match_opt(const std::vector<std::string>& a, std::size_t& i, const std::string& flag, std::string& value) {
    const std::string& cur = a[i];
    if (cur == flag) {
        if (i + 1 >= a.size()) throw UsageError("option " + flag + " requires a value");
        value = a[++i];
        return true;
    }
    if (cur.rfind(flag + "=", 0) == 0) {
        value = cur.substr(flag.size() + 1);
        return true;
    }
    return false;
}

int parse_int(const std::string& s, const std::string& what, int lo, int hi) {
    try {
        std::size_t used = 0;
        long v = std::stol(s, &used);
        if (used != s.size() || v < lo || v > hi) throw std::out_of_range("");
        return static_cast<int>(v);
    } catch (const std::exception&) {
        throw UsageError(what + " must be an integer between " + std::to_string(lo) + " and " + std::to_string(hi) + " (got '" + s + "')");
    }
}

int cmd_run(const std::vector<std::string>& a) {
    ContainerConfig c;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::string& cur = a[i];
        std::string v;
        if (cur == "--") {
            c.command.assign(a.begin() + static_cast<long>(i) + 1, a.end());
            break;
        } else if (match_opt(a, i, "--name", v)) c.name = v;
        else if (match_opt(a, i, "--rootfs", v)) c.rootfs = v;
        else if (match_opt(a, i, "--hostname", v)) c.hostname = v;
        else if (match_opt(a, i, "--memory", v)) {
            try { c.memory_bytes = parse_size(v); } catch (const std::invalid_argument& e) { throw UsageError(e.what()); }
        } else if (match_opt(a, i, "--cpu", v)) c.cpu_percent = parse_int(v, "--cpu", 1, 100000);
        else if (match_opt(a, i, "--pids", v)) c.pids_max = parse_int(v, "--pids", 1, 4194304);
        else if (match_opt(a, i, "--grace", v)) c.grace_seconds = parse_int(v, "--grace", 0, 3600);
        else if (match_opt(a, i, "--stats-file", v)) c.stats_file = v;
        else if (match_opt(a, i, "--caps", v)) {
            if (v == "default") c.caps = CapsMode::Default;
            else if (v == "strict") c.caps = CapsMode::Strict;
            else if (v == "all") c.caps = CapsMode::All;
            else throw UsageError("--caps must be default, strict or all");
        } else if (cur == "--isolate-net") c.isolate_net = true;
        else if (cur == "-d" || cur == "--detach") c.detach = true;
        else if (cur == "--no-init") c.no_init = true;
        else if (cur == "-q" || cur == "--quiet") c.quiet = true;
        else throw UsageError("unknown option '" + cur + "' (options go before '--')");
    }
    if (c.rootfs.empty()) throw UsageError("--rootfs is required");
    if (c.command.empty()) throw UsageError("missing command: minicontainer run [options] -- <command> [args...]");
    return Container(c).run();
}

std::string uptime_str(std::int64_t started) {
    std::int64_t now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::int64_t d = now - started;
    if (d < 0) d = 0;
    char buf[32];
    if (d < 60) std::snprintf(buf, sizeof buf, "%llds", static_cast<long long>(d));
    else if (d < 3600) std::snprintf(buf, sizeof buf, "%lldm%02llds", static_cast<long long>(d / 60), static_cast<long long>(d % 60));
    else std::snprintf(buf, sizeof buf, "%lldh%02lldm", static_cast<long long>(d / 3600), static_cast<long long>((d % 3600) / 60));
    return buf;
}

int cmd_ps(const std::vector<std::string>&) {
    auto all = Registry::list();
    int stale = 0;
    std::cout << std::left << std::setw(20) << "NAME" << std::setw(9) << "PID" << std::setw(10) << "UPTIME"
              << std::setw(12) << "MEMORY" << "COMMAND\n";
    for (const auto& s : all) {
        if (!Registry::is_running(s)) { ++stale; continue; }
        auto stats = ResourceManager::read_stats_at(s.cgroup);
        std::cout << std::left << std::setw(20) << s.name << std::setw(9) << s.pid << std::setw(10) << uptime_str(s.started)
                  << std::setw(12) << human_bytes(stats.memory_current) << s.command << "\n";
    }
    if (stale) std::cerr << stale << " stale record(s) found; run 'minicontainer clean'\n";
    return 0;
}

int cmd_inspect(const std::vector<std::string>& a) {
    if (a.size() != 1) throw UsageError("usage: minicontainer inspect <name>");
    auto st = Registry::load(a[0]);
    if (!st) throw ExitError(1, "no such container: " + a[0]);
    const bool running = Registry::is_running(*st);
    std::cout << "Container:    " << st->name << "\n"
              << "Status:       " << (running ? "running (up " + uptime_str(st->started) + ")" : std::string("exited (stale record)")) << "\n"
              << "Runner PID:   " << st->runner_pid << "\n"
              << "Host PID:     " << st->pid << "   (PID 1 inside the container)\n"
              << "Hostname:     " << st->hostname << "\n"
              << "Rootfs:       " << st->rootfs << "\n"
              << "Command:      " << st->command << "\n"
              << "Detached:     " << (st->detached ? "yes" : "no") << "\n"
              << "Cgroup:       " << st->cgroup << "\n";
    if (!running) return 0;
    std::cout << "\nNamespaces (container vs. host):\n";
    for (const auto& n : NamespaceManager::describe(st->pid))
        std::cout << "  " << std::left << std::setw(5) << n.name << std::setw(24) << n.id << (n.isolated ? "isolated" : "shared with host")
                  << "   (host: " << n.host_id << ")\n";
    const auto s = ResourceManager::read_stats_at(st->cgroup);
    std::cout << "\nResources:\n";
    std::cout << "  memory.current  " << human_bytes(s.memory_current) << "\n";
    std::cout << "  memory.max      " << (s.memory_max == "max" || s.memory_max.empty() ? "unlimited" : human_bytes(std::stoull(s.memory_max)))
              << "  (configured: " << (st->memory_bytes ? human_bytes(st->memory_bytes) : "unlimited") << ")\n";
    if (s.memory_peak) std::cout << "  memory.peak     " << human_bytes(*s.memory_peak) << "\n";
    std::cout << "  oom_kill        " << s.oom_kill_events << "\n";
    std::cout << "  cpu.max         " << s.cpu_max << "  (configured: " << (st->cpu_percent ? std::to_string(st->cpu_percent) + "%" : "unlimited") << ")\n";
    char buf[160];
    std::snprintf(buf, sizeof buf, "  cpu usage       %.3fs (user %.3fs, system %.3fs), throttled %llu periods\n", s.cpu_usage_usec / 1e6,
                  s.cpu_user_usec / 1e6, s.cpu_system_usec / 1e6, static_cast<unsigned long long>(s.nr_throttled));
    std::cout << buf;
    if (s.pids_current) std::cout << "  pids.current    " << *s.pids_current << "  (max: " << s.pids_max << ")\n";
    return 0;
}

int cmd_stop(const std::vector<std::string>& a) {
    if (a.size() != 1) throw UsageError("usage: minicontainer stop <name>");
    auto st = Registry::load(a[0]);
    if (!st) throw ExitError(1, "no such container: " + a[0]);
    if (!Registry::is_running(*st)) {
        ResourceManager::remove_cgroup(st->cgroup);
        Registry::remove(st->name);
        std::cout << st->name << " was not running; stale resources removed\n";
        return 0;
    }
    // Ask the runner: it forwards SIGTERM to the container, escalates to SIGKILL after the grace period,
    // and then performs the normal cgroup/registry cleanup.
    ::kill(st->runner_pid, SIGTERM);
    auto wait_gone = [&](int ms) {
        for (int i = 0; i < ms / 100; ++i) {
            if (!Registry::process_alive(st->runner_pid)) return true;
            ::usleep(100000);
        }
        return !Registry::process_alive(st->runner_pid);
    };
    if (!wait_gone((st->grace + 3) * 1000)) {
        ::kill(st->pid, SIGKILL);  // runner then reaps it and cleans up
        wait_gone(3000);
    }
    if (Registry::process_alive(st->runner_pid)) throw ExitError(1, "container '" + st->name + "' did not stop");
    std::cout << st->name << "\n";
    return 0;
}

int cmd_logs(const std::vector<std::string>& a) {
    if (a.size() != 1) throw UsageError("usage: minicontainer logs <name>");
    auto text = read_file(Registry::log_path(a[0]));
    if (!text) throw ExitError(1, "no logs for container: " + a[0] + " (only detached containers keep logs)");
    std::cout << *text;
    return 0;
}

int cmd_clean(const std::vector<std::string>&) {
    int states = 0, groups = 0;
    std::vector<std::string> live;
    for (const auto& s : Registry::list()) {
        if (Registry::is_running(s)) { live.push_back(s.name); continue; }
        ResourceManager::remove_cgroup(s.cgroup);
        Registry::remove(s.name);
        ++states;
    }
    const std::string parent = ResourceManager::cgroup_root() + "/" + ResourceManager::kParentName;
    if (DIR* d = ::opendir(parent.c_str())) {
        while (dirent* e = ::readdir(d)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            std::string p = parent + "/" + n;
            struct stat sb;
            if (::stat(p.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) continue;
            bool is_live = false;
            for (auto& l : live) is_live |= (l == n);
            if (is_live) continue;
            ResourceManager::remove_cgroup(p);
            ++groups;
        }
        ::closedir(d);
    }
    // Logs of detached containers are kept after they exit (for post-mortems); `clean` discards the old ones.
    int logs = 0;
    if (DIR* d = ::opendir(Registry::dir().c_str())) {
        while (dirent* e = ::readdir(d)) {
            std::string n = e->d_name;
            if (n.size() <= 4 || n.compare(n.size() - 4, 4, ".log") != 0) continue;
            const std::string base = n.substr(0, n.size() - 4);
            bool is_live = false;
            for (auto& l : live) is_live |= (l == base);
            if (!is_live && ::unlink((Registry::dir() + "/" + n).c_str()) == 0) ++logs;
        }
        ::closedir(d);
    }
    std::cout << "removed " << states << " stale record(s), " << groups << " orphan cgroup(s), " << logs << " old log(s)\n";
    return 0;
}

}  // namespace

int run_cli(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    try {
        if (args.empty()) {
            std::cerr << kUsage;
            return 2;
        }
        const std::string cmd = args[0];
        std::vector<std::string> rest(args.begin() + 1, args.end());
        if (cmd == "--help" || cmd == "-h" || cmd == "help") { std::cout << kUsage; return 0; }
        if (cmd == "--version" || cmd == "version") { std::cout << kVersion << "\n"; return 0; }
        if (cmd == "run") return cmd_run(rest);
        if (cmd == "ps") return cmd_ps(rest);
        if (cmd == "inspect") return cmd_inspect(rest);
        if (cmd == "stop") return cmd_stop(rest);
        if (cmd == "logs") return cmd_logs(rest);
        if (cmd == "clean") return cmd_clean(rest);
        throw UsageError("unknown command '" + cmd + "'");
    } catch (const UsageError& e) {
        std::cerr << "minicontainer: " << e.what() << "\nTry 'minicontainer --help'.\n";
        return 2;
    } catch (const ExitError& e) {
        std::cerr << "minicontainer: error: " << e.what() << "\n";
        return e.code();
    } catch (const std::exception& e) {
        std::cerr << "minicontainer: error: " << e.what() << "\n";
        return 125;
    }
}

}  // namespace mc
