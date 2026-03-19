// logv WebAssembly binding — exposes log line parsing via Emscripten Embind.
// Compiled with:  emcc logv_wasm.cpp log_parser.cpp -I../../logvcore/include
//                 -std=c++17 -O2 -lembind -sSINGLE_FILE=1
//                 --shell-file ../shell.html -o ../dist/logv.html

#include <emscripten/bind.h>
#include <string>
#include <cstring>
#include "logvcore/log_parser.h"

using namespace emscripten;

struct ParsedLine {
    std::string ts;
    std::string svc;
    std::string lvl;
    std::string msg;
    std::string raw;
    bool        valid;
};

ParsedLine parse_line_wrap(const std::string& raw_line) {
    logvcore::LogEntry e = logvcore::parse_line(raw_line);
    ParsedLine p;
    p.ts    = e.timestamp;
    p.svc   = e.service;
    p.lvl   = e.level;
    p.msg   = e.message;
    p.raw   = e.raw;
    p.valid = e.valid();
    return p;
}

EMSCRIPTEN_BINDINGS(logv) {
    value_object<ParsedLine>("ParsedLine")
        .field("ts",    &ParsedLine::ts)
        .field("svc",   &ParsedLine::svc)
        .field("lvl",   &ParsedLine::lvl)
        .field("msg",   &ParsedLine::msg)
        .field("raw",   &ParsedLine::raw)
        .field("valid", &ParsedLine::valid);

    function("parseLine", &parse_line_wrap);
}
