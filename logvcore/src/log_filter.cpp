#include "logvcore/log_filter.h"
#include <cstring>
#include <algorithm>
#include <cctype>

namespace logvcore {

namespace {

std::string to_lower(const std::string& s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    return out;
}

// Find index of name in a fixed registry; returns 64 (invalid) if not found.
uint8_t find_index(const std::string* names, int count, const char* name) {
    for (int i = 0; i < count; ++i)
        if (names[i] == name) return static_cast<uint8_t>(i);
    return 64;
}

} // anonymous namespace

void LogFilter::set_services(const std::vector<std::string>& names) {
    svc_count_ = 0;
    svc_mask_ = 0;
    for (auto& n : names) {
        if (svc_count_ >= 65) break;
        svc_names_[svc_count_] = n;
        svc_mask_ |= (uint64_t{1} << svc_count_);
        ++svc_count_;
    }
    if (names.empty()) svc_mask_ = ~uint64_t{0};  // empty = accept all
}

void LogFilter::set_levels(const std::vector<std::string>& names) {
    lvl_count_ = 0;
    lvl_mask_ = 0;
    for (auto& n : names) {
        if (lvl_count_ >= 65) break;
        lvl_names_[lvl_count_] = n;
        lvl_mask_ |= (uint64_t{1} << lvl_count_);
        ++lvl_count_;
    }
    if (names.empty()) lvl_mask_ = ~uint64_t{0};  // empty = accept all
}

void LogFilter::set_text(const std::string& substr) {
    text_ = to_lower(substr);
}

bool LogFilter::accepts(const LogEntry& e) const noexcept {
    // Service check
    if (svc_count_ > 0) {
        uint8_t idx = find_index(svc_names_, svc_count_, e.service);
        if (idx > 64 || !((svc_mask_ >> idx) & 1))
            return false;
    }

    // Level check
    if (lvl_count_ > 0) {
        uint8_t idx = find_index(lvl_names_, lvl_count_, e.level);
        if (idx > 64 || !((lvl_mask_ >> idx) & 1))
            return false;
    }

    // Text check — case-insensitive substring in message
    if (!text_.empty()) {
        // Fast path: manual tolower + search
        const auto& msg = e.message;
        if (msg.size() < text_.size()) return false;
        auto it = std::search(
            msg.begin(), msg.end(),
            text_.begin(), text_.end(),
            [](unsigned char a, unsigned char b){
                return std::tolower(a) == b;  // b is already lowercase
            });
        if (it == msg.end()) return false;
    }

    return true;
}

std::size_t LogFilter::filter(const std::vector<LogEntry>& in,
                               std::vector<const LogEntry*>& out) const {
    std::size_t count = 0;
    for (const auto& e : in) {
        if (accepts(e)) {
            out.push_back(&e);
            ++count;
        }
    }
    return count;
}

uint8_t LogFilter::service_index(const char* name) const {
    return find_index(svc_names_, svc_count_, name);
}

uint8_t LogFilter::level_index(const char* name) const {
    return find_index(lvl_names_, lvl_count_, name);
}

} // namespace logvcore
