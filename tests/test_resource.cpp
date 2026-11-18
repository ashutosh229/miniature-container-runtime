#include <unistd.h>

#include "common/util.hpp"
#include "resource/resource_manager.hpp"
#include "test_framework.hpp"

using namespace mc;

int main() {
    // --- pure functions (no root needed) ---
    CHECK_EQ(parse_size("100M"), 104857600ULL);
    CHECK_EQ(parse_size("1G"), 1073741824ULL);
    CHECK_EQ(parse_size("512k"), 524288ULL);
    CHECK_EQ(parse_size("4096"), 4096ULL);
    CHECK_EQ(parse_size("2MiB"), 2097152ULL);
    CHECK_THROWS(parse_size(""));
    CHECK_THROWS(parse_size("abc"));
    CHECK_THROWS(parse_size("10X"));
    CHECK_THROWS(parse_size("99999999999999999999T"));
    CHECK_EQ(ResourceManager::cpu_max_value(50), std::string("50000 100000"));
    CHECK_EQ(ResourceManager::cpu_max_value(100), std::string("100000 100000"));
    CHECK_EQ(ResourceManager::cpu_max_value(25), std::string("25000 100000"));
    CHECK_EQ(human_bytes(104857600), std::string("100.0 MiB"));

    auto kv = parse_kv("usage_usec 1234\nuser_usec 1000\nname abc\nnr_throttled 7\n");
    CHECK_EQ(kv["usage_usec"], 1234ULL);
    CHECK_EQ(kv["nr_throttled"], 7ULL);
    CHECK(kv.find("name") == kv.end());

    CHECK_THROWS(ResourceManager::validate_limits({1024, 0, 0}));        // < 1 MiB
    CHECK_THROWS(ResourceManager::validate_limits({0, -5, 0}));
    CHECK_THROWS(ResourceManager::validate_limits({0, 100000000, 0}));   // far above ncpu*100
    ResourceManager::validate_limits({100ULL << 20, 50, 64});             // ok, must not throw

    CHECK(valid_name("web-1.test_x"));
    CHECK(!valid_name("../etc"));
    CHECK(!valid_name(""));
    CHECK(!valid_name("a/b"));
    CHECK(!valid_name(".hidden"));

    // --- real cgroup test (root + cgroup v2 only) ---
    if (geteuid() != 0) {
        SKIP("cgroup integration test needs root");
        return finish("test_resource");
    }
    try {
        ResourceLimits lim{104857600ULL, 50, 64};
        ResourceManager::prepare_hierarchy(lim);
        std::string path;
        {
            ResourceManager rm("mc-unit-test");
            ResourceManager::remove_cgroup(rm.path());
            rm.create(lim);
            path = rm.path();
            auto rd = [&](const char* f) { auto t = read_file(path + "/" + f); return t ? trim(*t) : std::string("<missing>"); };
            CHECK_EQ(rd("memory.max"), std::string("104857600"));
            CHECK_EQ(rd("cpu.max"), std::string("50000 100000"));
            CHECK_EQ(rd("pids.max"), std::string("64"));
            auto s = rm.read_stats();
            CHECK_EQ(s.memory_max, std::string("104857600"));
            CHECK_EQ(s.cpu_max, std::string("50000 100000"));
            CHECK(path_exists(path));
        }
        CHECK(!path_exists(path));  // destructor removed the cgroup
    } catch (const std::exception& e) {
        SKIP(std::string("cgroup v2 unavailable here: ") + e.what());
    }
    return finish("test_resource");
}
