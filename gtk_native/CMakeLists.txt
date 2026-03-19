cmake_minimum_required(VERSION 3.16)
project(logv_gtk_native CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)

# Prefer GTK3 for broad availability on WSL/Linux.
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_check_modules(JSONCPP REQUIRED jsoncpp)

add_executable(logv-gtk
    src/main.cpp
    ../logvcore/src/log_parser.cpp
)

target_include_directories(logv-gtk PRIVATE
    ${GTK3_INCLUDE_DIRS}
    ${JSONCPP_INCLUDE_DIRS}
    ../logvcore/include
)

target_compile_options(logv-gtk PRIVATE
    ${GTK3_CFLAGS_OTHER}
    ${JSONCPP_CFLAGS_OTHER}
    -Wall -Wextra -Wpedantic
)

target_link_directories(logv-gtk PRIVATE ${GTK3_LIBRARY_DIRS} ${JSONCPP_LIBRARY_DIRS})

target_link_libraries(logv-gtk PRIVATE ${GTK3_LIBRARIES} ${JSONCPP_LIBRARIES})
