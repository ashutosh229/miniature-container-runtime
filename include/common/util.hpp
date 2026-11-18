#pragma once
// Small helpers shared by every module: errors, parsing, file I/O.
#include <sys/types.h>

#include <cerrno>
#include <cstdint>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mc {

// std::runtime_error that appends strerror(err) to the message.
class SysError : public std::runtime_error {
public:
    explicit SysError(const std::string& what, int err = errno);
    int error() const noexcept { return err_; }

private:
    int err_;
};

// An error that should terminate the CLI with a specific exit status
// (e.g. 127 = command not found, mirroring shell/docker conventions).
class ExitError : public std::runtime_error {
public:
    ExitError(int code, const std::string& what) : std::runtime_error(what), code_(code) {}
    int code() const noexcept { return code_; }

private:
    int code_;
};

std::string trim(const std::string& s);
std::string join(const std::vector<std::string>& parts, const std::string& sep);

// "100M" -> 104857600. Accepts K/M/G/T (optionally with B/iB), plain bytes.
std::uint64_t parse_size(const std::string& text);
std::string human_bytes(std::uint64_t bytes);
bool valid_name(const std::string& name);
std::string random_hex(std::size_t nbytes);

std::optional<std::string> read_file(const std::string& path);
// Writes `value` with ONE write() call (required by cgroup control files).
void write_control_file(const std::string& path, const std::string& value);
// Atomically (tmp + rename) writes a regular text file.
void write_text_file(const std::string& path, const std::string& content);
void mkdir_p(const std::string& path, mode_t mode = 0755);
bool path_exists(const std::string& path);

// Parses "key value\n" lines (cpu.stat, memory.events). Non-numeric lines are skipped.
std::map<std::string, std::uint64_t> parse_kv(const std::string& text);

}  // namespace mc
