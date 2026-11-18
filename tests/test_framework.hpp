#pragma once
// Minimal dependency-free test helpers (no GoogleTest needed).
#include <iostream>

inline int& mc_checks() { static int n = 0; return n; }
inline int& mc_failures() { static int n = 0; return n; }

#define CHECK(cond)                                                                      \
    do {                                                                                 \
        ++mc_checks();                                                                   \
        if (!(cond)) {                                                                   \
            ++mc_failures();                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: " #cond "\n";   \
        }                                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        ++mc_checks();                                                                                     \
        auto _va = (a);                                                                                    \
        auto _vb = (b);                                                                                    \
        if (!(_va == _vb)) {                                                                               \
            ++mc_failures();                                                                               \
            std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK_EQ failed: " #a " == " #b "  (" << _va    \
                      << " vs " << _vb << ")\n";                                                           \
        }                                                                                                  \
    } while (0)

#define CHECK_THROWS(expr)                                                               \
    do {                                                                                 \
        ++mc_checks();                                                                   \
        bool _thrown = false;                                                            \
        try { expr; } catch (...) { _thrown = true; }                                    \
        if (!_thrown) {                                                                  \
            ++mc_failures();                                                             \
            std::cerr << __FILE__ << ":" << __LINE__ << ": expected exception: " #expr "\n"; \
        }                                                                                \
    } while (0)

#define SKIP(msg) std::cout << "  SKIP: " << msg << "\n"

inline int finish(const char* name) {
    std::cout << name << ": " << (mc_checks() - mc_failures()) << "/" << mc_checks() << " checks passed\n";
    return mc_failures() ? 1 : 0;
}
