#pragma once
#include "log_entry.h"
#include <string>

namespace logvcore {

/// Parse a single raw journalctl/syslog line into a LogEntry.
/// Handles both oclea structured format and plain syslog lines.
/// All work happens in a pre-allocated LogEntry to avoid extra allocations.
LogEntry parse_line(const std::string& raw);

} // namespace logvcore
