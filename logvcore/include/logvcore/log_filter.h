#pragma once
#include "log_entry.h"
#include <string>
#include <cstdint>
#include <vector>

namespace logvcore {

/// Compiled filter state — cheap to copy, hot path is fully inline.
/// Service and level membership are checked via 64-bit bitmasks
/// (up to 64 distinct values each) — single AND instruction per entry.
class LogFilter {
public:
    // Empty sets = "show all" (passthrough)
    LogFilter() = default;

    // --- build API ---
    void set_services(const std::vector<std::string>& names);
    void set_levels(const std::vector<std::string>& names);
    void set_text(const std::string& substr);   // case-insensitive

    void clear_services() noexcept { svc_mask_ = ~uint64_t{0}; svc_count_ = 0; }
    void clear_levels()   noexcept { lvl_mask_ = ~uint64_t{0}; lvl_count_ = 0; }
    void clear_text()     noexcept { text_.clear(); }

    // --- hot path ---
    bool accepts(const LogEntry& e) const noexcept;

    // Batch: writes accepted entries to `out`, returns count.
    std::size_t filter(const std::vector<LogEntry>& in,
                       std::vector<const LogEntry*>& out) const;

    // Stateless helper used by bindings to build service/level registries
    uint8_t service_index(const char* name) const;
    uint8_t level_index(const char* name) const;

private:
    uint64_t svc_mask_{~uint64_t{0}};   // bit i set = service i accepted
    uint64_t lvl_mask_{~uint64_t{0}};
    int      svc_count_{0};
    int      lvl_count_{0};
    std::string text_;  // lowercase

    // Registry: maps name → bit index (max 64 each)
    std::string svc_names_[64];
    std::string lvl_names_[64];
};

} // namespace logvcore
