#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/functional.h>

#include "logvcore/log_entry.h"
#include "logvcore/log_parser.h"
#include "logvcore/log_filter.h"
#include "logvcore/ring_buffer.h"

namespace py = pybind11;
using namespace logvcore;

// The shared ring buffer instance — single producer (SSH thread via push_raw),
// single consumer (Qt timer via drain).
// 65536 slots × ~200 bytes avg LogEntry ≈ 13 MB worst-case resident.
static SPSCRingBuffer<LogEntry, 65536> g_ring;

PYBIND11_MODULE(_logvcore, m) {
    m.doc() = "logvcore — C++ log parsing, filtering and ring buffer";

    // ---------------------------------------------------------------- LogEntry
    py::class_<LogEntry>(m, "LogEntry")
        .def(py::init<>())
        .def_property_readonly("timestamp", [](const LogEntry& e){ return std::string(e.timestamp); })
        .def_property_readonly("service",   [](const LogEntry& e){ return std::string(e.service); })
        .def_property_readonly("level",     [](const LogEntry& e){ return std::string(e.level); })
        .def_readonly("message", &LogEntry::message)
        .def_readonly("raw",     &LogEntry::raw)
        .def("valid", &LogEntry::valid)
        .def("__repr__", [](const LogEntry& e){
            return std::string(e.timestamp) + " [" + e.level + "] " + e.service + ": " + e.message;
        });

    // ---------------------------------------------------------------- parser
    m.def("parse_line", &parse_line, py::arg("raw"),
          "Parse a raw journalctl line into a LogEntry");

    // ---------------------------------------------------------------- LogFilter
    py::class_<LogFilter>(m, "LogFilter")
        .def(py::init<>())
        .def("set_services", &LogFilter::set_services, py::arg("names"))
        .def("set_levels",   &LogFilter::set_levels,   py::arg("names"))
        .def("set_text",     &LogFilter::set_text,      py::arg("substr"))
        .def("clear_services", &LogFilter::clear_services)
        .def("clear_levels",   &LogFilter::clear_levels)
        .def("clear_text",     &LogFilter::clear_text)
        .def("accepts", &LogFilter::accepts, py::arg("entry"))
        .def("filter", [](const LogFilter& f, const std::vector<LogEntry>& entries) {
            std::vector<const LogEntry*> ptrs;
            ptrs.reserve(entries.size());
            f.filter(entries, ptrs);
            // Return copies so Python owns the objects
            std::vector<LogEntry> out;
            out.reserve(ptrs.size());
            for (auto* p : ptrs) out.push_back(*p);
            return out;
        }, py::arg("entries"), "Filter a list of LogEntry, returning accepted entries");

    // ---------------------------------------------------------------- ring buffer
    // Exposed as module-level functions acting on the global singleton.
    // This keeps the hot push path free of Python object overhead.
    m.def("push_raw", [](const std::string& raw) -> bool {
        // Parse on producer side (SSH thread) — releases GIL during regex work
        LogEntry e = parse_line(raw);
        return g_ring.push(std::move(e));
    }, py::arg("raw"), py::call_guard<py::gil_scoped_release>(),
       "Parse and push one raw line. Returns False if buffer full (line dropped).");

    m.def("drain", [](std::size_t max_count) -> std::vector<LogEntry> {
        std::vector<LogEntry> out;
        out.reserve(std::min(max_count, std::size_t{512}));
        g_ring.pop_many(out, max_count);
        return out;
    }, py::arg("max_count") = 2048,
       "Drain up to max_count entries from the ring buffer. Call from Qt timer thread.");

    m.def("ring_size", []() -> std::size_t {
        return g_ring.size_approx();
    }, "Approximate number of entries currently in the ring buffer.");

    m.def("ring_capacity", []() -> std::size_t {
        return SPSCRingBuffer<LogEntry, 65536>::capacity();
    });

    m.def("ring_clear", []() {
        g_ring.clear();
    }, "Reset the ring buffer, discarding all contents.");
}
