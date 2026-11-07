cmake_minimum_required(VERSION 3.20)
project(UltraZIP VERSION 3.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release build if not specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Dependencies ──
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBARCHIVE REQUIRED IMPORTED_TARGET libarchive)
pkg_check_modules(ZSTD      REQUIRED IMPORTED_TARGET libzstd)
pkg_check_modules(LIBLZMA   REQUIRED IMPORTED_TARGET liblzma)

find_package(Qt6 REQUIRED COMPONENTS Widgets)
qt_standard_project_setup()

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# ── Target ──
add_executable(uzip WIN32
    src/main.cpp
    src/MainWindow.cpp
    src/uzip_format.cpp
    src/archiver.cpp
)

# Ensure headers are found correctly
target_include_directories(uzip PRIVATE src)

target_link_libraries(uzip PRIVATE
    PkgConfig::LIBARCHIVE
    PkgConfig::ZSTD
    PkgConfig::LIBLZMA
    Qt6::Widgets
)

# ── Windows specific (CAB / FCI API) ──
if(WIN32)
    target_link_libraries(uzip PRIVATE cabinet)
endif()

# ── Compiler flags ──
if(MSVC)
    target_compile_options(uzip PRIVATE /W4 /O2 /utf-8)
else()
    target_compile_options(uzip PRIVATE -O2 -Wall -Wextra)
endif()