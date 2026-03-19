cmake_minimum_required(VERSION 3.18)
project(logvcore CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Release by default for speed; override with -DCMAKE_BUILD_TYPE=Debug
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# -O2 (not O3) + no LTO: keeps link fast for dev iteration.
# march=native omitted so the same binary runs on all team machines.
# Emscripten overrides these flags automatically when toolchain is active.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

# Disable LTO — pybind11 3.x enables it by default but it makes link slow.
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION OFF)

# Use ccache if available
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    message(STATUS "Using ccache: ${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
endif()

# ── Emscripten / WASM detection ───────────────────────────────────────────────
# When you later build with: emcmake cmake .. -DCMAKE_BUILD_TYPE=Release
# EMSCRIPTEN is set automatically. The pybind11 module target is desktop-only;
# the Wasm variant will link logvcore_static directly into a JS/WASM bundle.
if(EMSCRIPTEN)
    message(STATUS "Building for Emscripten/WASM — skipping Python extension")
endif()

# ── core library (always built — shared by PyQt desktop and future Wasm target)
add_library(logvcore_static STATIC
    src/log_parser.cpp
    src/log_filter.cpp
)
target_include_directories(logvcore_static PUBLIC include)
target_compile_options(logvcore_static PRIVATE -Wall -Wextra -Wpedantic)

# ── Windows cross-compile: standalone shared library (no Python dependency) ───
# Built with: cmake -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake ..
if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    message(STATUS "Building Windows DLL (MinGW cross-compile)")
    add_library(logvcore SHARED
        src/log_parser.cpp
        src/log_filter.cpp
    )
    target_include_directories(logvcore PUBLIC include)
    target_compile_options(logvcore PRIVATE -Wall -Wextra -Wpedantic)
    # Export all symbols for the DLL (simpler than __declspec annotations)
    set_target_properties(logvcore PROPERTIES
        WINDOWS_EXPORT_ALL_SYMBOLS ON
        PREFIX ""
        OUTPUT_NAME "logvcore"
    )
    install(TARGETS logvcore RUNTIME DESTINATION ${CMAKE_SOURCE_DIR}/..)
endif()

# ── pybind11 Python extension (native Linux/macOS only) ──────────────────────
if(NOT EMSCRIPTEN AND NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
find_package(Python3 REQUIRED COMPONENTS Interpreter Development)

execute_process(
    COMMAND "${Python3_EXECUTABLE}" -c "import pybind11; print(pybind11.get_cmake_dir())"
    OUTPUT_VARIABLE PYBIND11_CMAKE_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
list(APPEND CMAKE_PREFIX_PATH "${PYBIND11_CMAKE_DIR}")
find_package(pybind11 CONFIG REQUIRED)

pybind11_add_module(_logvcore src/bindings.cpp)
target_link_libraries(_logvcore PRIVATE logvcore_static)
target_include_directories(_logvcore PRIVATE include)

# Install the .so next to the Python sources so `import _logvcore` works
install(TARGETS _logvcore DESTINATION ${CMAKE_SOURCE_DIR}/..)
endif() # NOT EMSCRIPTEN AND NOT Windows

# ── Emscripten WASM target (future) ──────────────────────────────────────────
# Build with: emcmake cmake .. && emmake make logvcore_wasm
# This links the same C++ core into a standalone .wasm + JS glue.
if(EMSCRIPTEN)
    add_executable(logvcore_wasm src/wasm_main.cpp)
    target_link_libraries(logvcore_wasm PRIVATE logvcore_static)
    target_link_options(logvcore_wasm PRIVATE
        "-sEXPORTED_FUNCTIONS=[_parse_line_js,_filter_js]"
        "-sEXPORTED_RUNTIME_METHODS=[ccall,cwrap]"
        "-sALLOW_MEMORY_GROWTH=1"
        "--bind"   # Embind for C++ ↔ JS
    )
    set_target_properties(logvcore_wasm PROPERTIES SUFFIX ".js")
endif()