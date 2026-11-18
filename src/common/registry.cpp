#include "common/registry.hpp"

#include <dirent.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <sstream>

#include "common/util.hpp"

namespace mc {

std::string Registry::dir() {
    const char* env = std::getenv("MC_STATE_DIR");
    return (env && *env) ? env : "/run/minicontainer";
}

std::string Registry::state_path(const std::string& name) { return dir() + "/" + name + ".state"; }
std::string Registry::log_path(const std::string& name) { return dir() + "/" + name + ".log"; }

void Registry::save(const ContainerState& s) {
    mkdir_p(dir());
    std::ostringstream o;
    o << "name=" << s.name << "\n"
      << "pid=" << s.pid << "\n"
      << "runner_pid=" << s.runner_pid << "\n"
      << "cgroup=" << s.cgroup << "\n"
      << "rootfs=" << s.rootfs << "\n"
      << "hostname=" << s.hostname << "\n"
      << "command=" << s.command << "\n"
      << "memory_bytes=" << s.memory_bytes << "\n"
      << "cpu_percent=" << s.cpu_percent << "\n"
      << "pids_max=" << s.pids_max << "\n"
      << "grace=" << s.grace << "\n"
      << "isolate_net=" << (s.isolate_net ? 1 : 0) << "\n"
      << "detached=" << (s.detached ? 1 : 0) << "\n"
      << "started=" << s.started << "\n";
    write_text_file(state_path(s.name), o.str());
}

std::optional<ContainerState> Registry::load(const std::string& name) {
    if (!valid_name(name)) return std::nullopt;
    auto text = read_file(state_path(name));
    if (!text) return std::nullopt;
    ContainerState s;
    std::istringstream in(*text);
    std::string line;
    auto num = [](const std::string& v) -> long long {
        try { return std::stoll(v); } catch (...) { return 0; }
    };
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "name") s.name = v;
        else if (k == "pid") s.pid = static_cast<pid_t>(num(v));
        else if (k == "runner_pid") s.runner_pid = static_cast<pid_t>(num(v));
        else if (k == "cgroup") s.cgroup = v;
        else if (k == "rootfs") s.rootfs = v;
        else if (k == "hostname") s.hostname = v;
        else if (k == "command") s.command = v;
        else if (k == "memory_bytes") s.memory_bytes = static_cast<std::uint64_t>(num(v));
        else if (k == "cpu_percent") s.cpu_percent = static_cast<int>(num(v));
        else if (k == "pids_max") s.pids_max = static_cast<int>(num(v));
        else if (k == "grace") s.grace = static_cast<int>(num(v));
        else if (k == "isolate_net") s.isolate_net = num(v) != 0;
        else if (k == "detached") s.detached = num(v) != 0;
        else if (k == "started") s.started = num(v);
    }
    if (s.name.empty()) s.name = name;
    return s;
}

std::vector<ContainerState> Registry::list() {
    std::vector<ContainerState> out;
    DIR* d = ::opendir(dir().c_str());
    if (!d) return out;
    while (dirent* e = ::readdir(d)) {
        std::string f = e->d_name;
        const std::string suffix = ".state";
        if (f.size() > suffix.size() && f.compare(f.size() - suffix.size(), suffix.size(), suffix) == 0) {
            if (auto st = load(f.substr(0, f.size() - suffix.size()))) out.push_back(*st);
        }
    }
    ::closedir(d);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
    return out;
}

void Registry::remove(const std::string& name) noexcept {
    if (!valid_name(name)) return;
    ::unlink(state_path(name).c_str());
}

bool Registry::process_alive(pid_t pid) {
    if (pid <= 0) return false;
    if (::kill(pid, 0) != 0 && errno != EPERM) return false;
    // A zombie is not "running".
    if (auto stat = read_file("/proc/" + std::to_string(pid) + "/stat")) {
        auto rp = stat->rfind(')');
        if (rp != std::string::npos && rp + 2 < stat->size() && (*stat)[rp + 2] == 'Z') return false;
    }
    return true;
}

bool Registry::is_running(const ContainerState& st) { return process_alive(st.runner_pid); }

}  // namespace mc
