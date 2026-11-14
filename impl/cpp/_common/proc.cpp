#include "proc.hpp"

#include <sys/resource.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>

// Calls getrusage(RUSAGE_SELF); throws with the reason on failure.
static rusage readUsage() {
    rusage ru;
    if (getrusage(RUSAGE_SELF, &ru) != 0) {
        throw std::runtime_error(std::string("getrusage failed: ") + std::strerror(errno));
    }
    return ru;
}

ProcSnapshot procSnapshot() {
    rusage ru = readUsage();
    return {static_cast<int64_t>(ru.ru_utime.tv_sec) * 1000000000 + static_cast<int64_t>(ru.ru_utime.tv_usec) * 1000,
            static_cast<int64_t>(ru.ru_stime.tv_sec) * 1000000000 + static_cast<int64_t>(ru.ru_stime.tv_usec) * 1000,
            static_cast<int64_t>(ru.ru_nvcsw), static_cast<int64_t>(ru.ru_nivcsw)};
}

int64_t peakRssBytes() { return static_cast<int64_t>(readUsage().ru_maxrss) * 1024; }
