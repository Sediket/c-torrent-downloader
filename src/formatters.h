#pragma once

#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <string>

static inline void fmt_rate(double bps, char *buf, size_t len)
{
    if (bps >= 1024 * 1024)
        snprintf(buf, len, "%.1f MB/s", bps / (1024 * 1024));
    else if (bps >= 1024)
        snprintf(buf, len, "%.1f KB/s", bps / 1024);
    else
        snprintf(buf, len, "%.0f B/s", bps);
}

static inline void fmt_bytes(int64_t bytes, char *buf, size_t len)
{
    if (bytes >= 1024LL * 1024 * 1024)
        snprintf(buf, len, "%.2f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(buf, len, "%.1f MB", (double)bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(buf, len, "%.1f KB", (double)bytes / 1024.0);
    else
        snprintf(buf, len, "%" PRId64 " B", bytes);
}

static inline std::string url_encode(const std::string &s)
{
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}
