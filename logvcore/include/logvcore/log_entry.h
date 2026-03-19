#pragma once
#include <string>
#include <cstdint>

namespace logvcore {

/// A single parsed log line.
/// Fixed-size fields use char arrays to keep stack/cache cost low.
/// 'message' and 'raw' are heap strings — unavoidable for variable content.
struct LogEntry {
    char     timestamp[32]{};   // "Mar 17 12:34:56"
    char     service[64]{};     // "cloud-connection-service"
    char     level[12]{};       // "INFO" | "DEBUG" | "WARNING" | "ERROR" | "CRITICAL"
    std::string message;
    std::string raw;

    bool valid() const noexcept { return level[0] != '\0'; }
};

} // namespace logvcore
