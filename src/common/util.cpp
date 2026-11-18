#include "common/util.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

namespace mc {

SysError::SysError(const std::string& what, int err)
    : std::runtime_error(what + ": " + std::strerror(err)), err_(err) {}

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::uint64_t parse_size(const std::string& text) {
    const std::string s = trim(text);
    std::size_t i = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
    if (i == 0) throw std::invalid_argument("invalid size '" + text + "'");
    std::uint64_t value = 0;
    try {
        value = std::stoull(s.substr(0, i));
    } catch (...) {
        throw std::invalid_argument("size out of range: '" + text + "'");
    }
    std::string suffix = s.substr(i);
    for (auto& c : suffix) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    std::uint64_t mult = 1;
    if (suffix.empty() || suffix == "B") mult = 1;
    else if (suffix == "K" || suffix == "KB" || suffix == "KIB") mult = 1ULL << 10;
    else if (suffix == "M" || suffix == "MB" || suffix == "MIB") mult = 1ULL << 20;
    else if (suffix == "G" || suffix == "GB" || suffix == "GIB") mult = 1ULL << 30;
    else if (suffix == "T" || suffix == "TB" || suffix == "TIB") mult = 1ULL << 40;
    else throw std::invalid_argument("unknown size suffix in '" + text + "' (use K, M, G)");
    if (value > UINT64_MAX / mult) throw std::invalid_argument("size too large: '" + text + "'");
    return value * mult;
}

std::string human_bytes(std::uint64_t bytes) {
    static const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(bytes);
    int u = 0;
    while (v >= 1024.0 && u < 4) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    if (u == 0) std::snprintf(buf, sizeof buf, "%llu B", static_cast<unsigned long long>(bytes));
    else std::snprintf(buf, sizeof buf, "%.1f %s", v, units[u]);
    return buf;
}

bool valid_name(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    if (name[0] == '.' || name[0] == '-') return false;  // also rejects "." and ".."
    for (unsigned char c : name) {
        if (!(std::isalnum(c) || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

std::string random_hex(std::size_t nbytes) {
    std::random_device rd;
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (std::size_t i = 0; i < nbytes; ++i) {
        unsigned v = rd() & 0xff;
        out += digits[v >> 4];
        out += digits[v & 0xf];
    }
    return out;
}

std::optional<std::string> read_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    std::string out;
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) out.append(buf, static_cast<std::size_t>(n));
        else if (n == 0) break;
        else if (errno == EINTR) continue;
        else {
            ::close(fd);
            return std::nullopt;
        }
    }
    ::close(fd);
    return out;
}

void write_control_file(const std::string& path, const std::string& value) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) throw SysError("open " + path);
    ssize_t n = ::write(fd, value.data(), value.size());
    int e = errno;
    ::close(fd);
    if (n != static_cast<ssize_t>(value.size())) throw SysError("write '" + trim(value) + "' to " + path, e);
}

void write_text_file(const std::string& path, const std::string& content) {
    const std::string tmp = path + ".tmp";
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) throw SysError("open " + tmp);
    std::size_t off = 0;
    while (off < content.size()) {
        ssize_t n = ::write(fd, content.data() + off, content.size() - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            int e = errno;
            ::close(fd);
            ::unlink(tmp.c_str());
            throw SysError("write " + tmp, e);
        }
        off += static_cast<std::size_t>(n);
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        int e = errno;
        ::unlink(tmp.c_str());
        throw SysError("rename " + tmp, e);
    }
}

void mkdir_p(const std::string& path, mode_t mode) {
    std::string cur;
    std::size_t i = 0;
    while (i <= path.size()) {
        std::size_t j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        cur = path.substr(0, j);
        if (!cur.empty() && ::mkdir(cur.c_str(), mode) != 0 && errno != EEXIST) throw SysError("mkdir " + cur);
        i = j + 1;
    }
}

bool path_exists(const std::string& path) {
    struct stat sb;
    return ::stat(path.c_str(), &sb) == 0;
}

std::map<std::string, std::uint64_t> parse_kv(const std::string& text) {
    std::map<std::string, std::uint64_t> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string key, val;
        if (!(ls >> key >> val)) continue;
        try {
            std::size_t used = 0;
            std::uint64_t v = std::stoull(val, &used);
            if (used == val.size()) out[key] = v;
        } catch (...) {
        }
    }
    return out;
}

}  // namespace mc
