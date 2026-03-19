#include "logvcore/log_parser.h"
#include <cstring>
#include <cctype>

namespace logvcore {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------
namespace {

void copy_str(char* dst, std::size_t dst_size, const char* src, std::size_t len) {
    // Clamp to buffer size so we never read past `src + len`.
    std::size_t n = len < dst_size ? len : dst_size;
    std::memcpy(dst, src, n);
    dst[n] = '\0';
}

// Assign message, trimming trailing whitespace/newlines in-place.
void assign_message(LogEntry& e, const char* start, const char* end) {
    while (end > start &&
           (*(end - 1) == '\n' || *(end - 1) == '\r' ||
            *(end - 1) == ' '  || *(end - 1) == '\t')) {
        --end;
    }
    e.message.assign(start, end);
}

struct Level { const char* name; std::size_t len; };
static constexpr Level kLevels[] = {
    {"DEBUG",    5},
    {"INFO",     4},
    {"WARNING",  7},
    {"WARN",     4},
    {"ERROR",    5},
    {"CRITICAL", 8},
};

} // anonymous namespace

LogEntry parse_line(const std::string& raw) {
    LogEntry e;
    e.raw = raw;

    const char* p   = raw.c_str();
    const char* end = p + raw.size();

    // ── 1. Timestamp: "Mon DD HH:MM:SS[.usec]" ──────────────────────────────
    // Must start with an uppercase letter (month abbreviation).
    if (p >= end || !('A' <= *p && *p <= 'Z')) {
        assign_message(e, p, end);
        return e;
    }

    const char* ts_start = p;
    p += 3;  // skip 3-char month
    while (p < end && *p == ' ') ++p;
    if (p >= end || !isdigit(static_cast<unsigned char>(*p))) {
        assign_message(e, ts_start, end); return e;
    }
    while (p < end && isdigit(static_cast<unsigned char>(*p))) ++p;  // day
    if (p >= end || *p != ' ') { assign_message(e, ts_start, end); return e; }
    ++p;
    if (p >= end || !isdigit(static_cast<unsigned char>(*p))) {
        assign_message(e, ts_start, end); return e;
    }
    while (p < end && (isdigit(static_cast<unsigned char>(*p)) || *p == ':' || *p == '.')) ++p;
    copy_str(e.timestamp, sizeof(e.timestamp), ts_start, p - ts_start);

    // ── 2. Hostname ──────────────────────────────────────────────────────────
    if (p >= end || *p != ' ') return e;
    ++p;
    while (p < end && *p != ' ') ++p;  // skip hostname
    if (p >= end) return e;
    ++p;

    // ── 3. Service[PID]: ─────────────────────────────────────────────────────
    const char* svc_start = p;
    while (p < end && *p != '[' && *p != ':' && *p != ' ') ++p;
    std::size_t svc_len = static_cast<std::size_t>(p - svc_start);
    // Strip ".N" transient-unit suffix.
    for (std::size_t i = 0; i < svc_len; ++i) {
        if (svc_start[i] == '.') { svc_len = i; break; }
    }
    if (svc_len > 0)
        copy_str(e.service, sizeof(e.service), svc_start, svc_len);

    if (p < end && *p == '[') {
        while (p < end && *p != ']') ++p;
        if (p < end) ++p;
    }
    if (p < end && *p == ':') ++p;
    if (p < end && *p == ' ') ++p;

    // ── 4. Optional "<N>" kernel priority prefix ──────────────────────────────
    if (p < end && *p == '<') {
        while (p < end && *p != '>') ++p;
        if (p < end) ++p;
    }

    // ── 5. [LEVEL] ────────────────────────────────────────────────────────────
    if (p >= end || *p != '[') {
        assign_message(e, p, end);
        return e;
    }
    ++p;
    const char* lvl_start = p;
    while (p < end && *p != ']') ++p;
    const std::size_t lvl_len = static_cast<std::size_t>(p - lvl_start);
    if (p < end) ++p;  // skip ']'

    bool found_level = false;
    for (const auto& kl : kLevels) {
        if (kl.len == lvl_len && std::memcmp(lvl_start, kl.name, lvl_len) == 0) {
            // Normalise WARN → WARNING
            if (lvl_len == 4 && lvl_start[0] == 'W') {
                copy_str(e.level, sizeof(e.level), "WARNING", 7);
            } else {
                copy_str(e.level, sizeof(e.level), lvl_start, lvl_len);
            }
            found_level = true;
            break;
        }
    }
    if (!found_level) {
        assign_message(e, p, end);
        return e;
    }

    // ── 6. Optional (tid=N) ───────────────────────────────────────────────────
    if (p < end && *p == '(') {
        while (p < end && *p != ')') ++p;
        if (p < end) ++p;
    }
    while (p < end && *p == ' ') ++p;

    // ── 7. Message ────────────────────────────────────────────────────────────
    assign_message(e, p, end);
    return e;
}

} // namespace logvcore
