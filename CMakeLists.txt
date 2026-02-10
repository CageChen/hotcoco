cmake_minimum_required(VERSION 3.20)
project(hotcoco
    VERSION 0.1.0
    DESCRIPTION "A user-friendly C++20 coroutine library"
    LANGUAGES CXX C
)

# ============================================================================
# C++20 Configuration
# ============================================================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile commands for IDE support
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Default to Release if no build type specified
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ============================================================================
# Options
# ============================================================================
# BUILD_TESTS defaults ON for top-level builds, OFF when used via FetchContent
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    option(BUILD_TESTS "Build unit tests" ON)
    option(BUILD_EXAMPLES "Build example programs" ON)
else()
    option(BUILD_TESTS "Build unit tests" OFF)
    option(BUILD_EXAMPLES "Build example programs" OFF)
endif()
option(BUILD_BENCHMARKS "Build performance benchmarks" OFF)

# Sanitizer options
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)

# io_uring option (Linux 5.1+ only)
option(ENABLE_IOURING "Enable io_uring executor (Linux 5.1+)" ON)

# P2300 std::execution adapter option
option(ENABLE_STDEXEC "Enable P2300 std::execution adapters (requires stdexec)" OFF)

# Allocator options
option(USE_TCMALLOC "Use tcmalloc allocator" OFF)
option(USE_JEMALLOC "Use jemalloc allocator" OFF)
option(USE_MIMALLOC "Use mimalloc allocator" OFF)

# Check for conflicting sanitizers
if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "ASan and TSan cannot be used together")
endif()

# Check for conflicting allocators
set(ALLOCATOR_COUNT 0)
if(USE_TCMALLOC)
    math(EXPR ALLOCATOR_COUNT "${ALLOCATOR_COUNT} + 1")
endif()
if(USE_JEMALLOC)
    math(EXPR ALLOCATOR_COUNT "${ALLOCATOR_COUNT} + 1")
endif()
if(USE_MIMALLOC)
    math(EXPR ALLOCATOR_COUNT "${ALLOCATOR_COUNT} + 1")
endif()
if(ALLOCATOR_COUNT GREATER 1)
    message(FATAL_ERROR "Only one custom allocator can be enabled at a time")
endif()

# ============================================================================
# Dependencies
# ============================================================================
include(FetchContent)

# libuv for event loop - fetch and build if not found
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBUV QUIET libuv)
endif()

if(NOT LIBUV_FOUND)
    message(STATUS "libuv not found via pkg-config, fetching from source...")
    FetchContent_Declare(
        libuv
        GIT_REPOSITORY https://github.com/libuv/libuv.git
        GIT_TAG        v1.48.0
    )
    # Disable libuv tests and benchmarks
    set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libuv)
    
    set(LIBUV_LIBRARIES uv_a)
    set(LIBUV_INCLUDE_DIRS ${libuv_SOURCE_DIR}/include)
endif()

# llhttp for HTTP parsing
message(STATUS "Fetching llhttp...")
FetchContent_Declare(
    llhttp
    GIT_REPOSITORY https://github.com/nodejs/llhttp.git
    GIT_TAG        release/v9.2.1
)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(llhttp)

# liburing for io_uring support (Linux 5.1+)
set(HAVE_IOURING OFF)
if(ENABLE_IOURING AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBURING QUIET liburing)
    endif()
    
    if(LIBURING_FOUND)
        message(STATUS "liburing found: ${LIBURING_VERSION}")
        set(HAVE_IOURING ON)
    else()
        # Try to find liburing directly
        find_library(LIBURING_LIB NAMES uring)
        find_path(LIBURING_INCLUDE_DIR NAMES liburing.h)
        if(LIBURING_LIB AND LIBURING_INCLUDE_DIR)
            message(STATUS "liburing found via find_library")
            set(HAVE_IOURING ON)
            set(LIBURING_LIBRARIES ${LIBURING_LIB})
            set(LIBURING_INCLUDE_DIRS ${LIBURING_INCLUDE_DIR})
        else()
            message(STATUS "liburing not found, io_uring executor disabled")
        endif()
    endif()
endif()

# stdexec (P2300 reference implementation)
if(ENABLE_STDEXEC)
    message(STATUS "Fetching stdexec (P2300 std::execution)...")
    FetchContent_Declare(
        stdexec
        GIT_REPOSITORY https://github.com/NVIDIA/stdexec.git
        GIT_TAG 624efd64005660aca0b88e6b39ac825a6dd6135d  # Pin to known-good version
        GIT_SHALLOW TRUE
    )
    set(STDEXEC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(STDEXEC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(stdexec)
endif()


# ============================================================================
# Main Library
# ============================================================================
set(HOTCOCO_SOURCES
    src/core/error.cpp
    src/core/task.cpp
    src/http/http.cpp
    src/io/executor.cpp
    src/io/libuv_executor.cpp
    src/io/polling_executor.cpp
    src/io/sync_tcp.cpp
    src/io/tcp.cpp
    src/io/thread_pool_executor.cpp
    src/io/thread_utils.cpp
)

if(HAVE_IOURING)
    list(APPEND HOTCOCO_SOURCES src/io/iouring_executor.cpp)
    list(APPEND HOTCOCO_SOURCES src/io/iouring_tcp.cpp)
endif()

add_library(hotcoco ${HOTCOCO_SOURCES})

target_include_directories(hotcoco
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${LIBUV_INCLUDE_DIRS}>
        $<BUILD_INTERFACE:${llhttp_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(hotcoco
    PUBLIC
        ${LIBUV_LIBRARIES}
        llhttp_static
)

# io_uring configuration
if(HAVE_IOURING)
    target_include_directories(hotcoco PUBLIC ${LIBURING_INCLUDE_DIRS})
    target_link_libraries(hotcoco PUBLIC ${LIBURING_LIBRARIES})
    target_compile_definitions(hotcoco PUBLIC HOTCOCO_HAS_IOURING=1)
    message(STATUS "io_uring executor enabled")
endif()

# stdexec configuration
if(ENABLE_STDEXEC)
    target_link_libraries(hotcoco PUBLIC STDEXEC::stdexec)
    target_compile_definitions(hotcoco PUBLIC HOTCOCO_HAS_STDEXEC=1)
    message(STATUS "P2300 std::execution adapters enabled")
endif()

target_compile_options(hotcoco
    PRIVATE
        -Wall -Wextra -Wpedantic
        -fno-exceptions
)

# ============================================================================
# Sanitizer Configuration
# ============================================================================
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    target_compile_options(hotcoco PUBLIC -fsanitize=address -fno-omit-frame-pointer -g)
    target_link_options(hotcoco PUBLIC -fsanitize=address)
endif()

if(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    target_compile_options(hotcoco PUBLIC -fsanitize=thread -g)
    target_link_options(hotcoco PUBLIC -fsanitize=thread)
endif()

if(ENABLE_UBSAN)
    message(STATUS "UndefinedBehaviorSanitizer enabled")
    target_compile_options(hotcoco PUBLIC -fsanitize=undefined -g)
    target_link_options(hotcoco PUBLIC -fsanitize=undefined)
endif()

# ============================================================================
# Allocator Configuration
# ============================================================================
if(USE_TCMALLOC)
    find_library(TCMALLOC_LIB NAMES tcmalloc_minimal tcmalloc)
    if(TCMALLOC_LIB)
        message(STATUS "tcmalloc enabled: ${TCMALLOC_LIB}")
        target_link_libraries(hotcoco PUBLIC ${TCMALLOC_LIB})
    else()
        message(FATAL_ERROR "tcmalloc not found. Install gperftools or disable USE_TCMALLOC")
    endif()
endif()

if(USE_JEMALLOC)
    find_library(JEMALLOC_LIB NAMES jemalloc)
    if(JEMALLOC_LIB)
        message(STATUS "jemalloc enabled: ${JEMALLOC_LIB}")
        target_link_libraries(hotcoco PUBLIC ${JEMALLOC_LIB})
    else()
        message(FATAL_ERROR "jemalloc not found. Install jemalloc or disable USE_JEMALLOC")
    endif()
endif()

if(USE_MIMALLOC)
    find_package(mimalloc CONFIG)
    if(mimalloc_FOUND)
        message(STATUS "mimalloc enabled")
        target_link_libraries(hotcoco PUBLIC mimalloc)
    else()
        # Fallback to library search
        find_library(MIMALLOC_LIB NAMES mimalloc)
        if(MIMALLOC_LIB)
            message(STATUS "mimalloc enabled: ${MIMALLOC_LIB}")
            target_link_libraries(hotcoco PUBLIC ${MIMALLOC_LIB})
        else()
            message(FATAL_ERROR "mimalloc not found. Install mimalloc or disable USE_MIMALLOC")
        endif()
    endif()
endif()

# ============================================================================
# Tests
# ============================================================================
if(BUILD_TESTS)
    enable_testing()
    
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.tar.gz
        URL_HASH SHA256=8ad598c73ad796e0d8280b082cebd82a630d73e73cd3c70057938a6501bba5d7
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    
    add_subdirectory(tests)
endif()

# ============================================================================
# Examples
# ============================================================================
if(BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# ============================================================================
# Benchmarks
# ============================================================================
if(BUILD_BENCHMARKS)
    FetchContent_Declare(
        benchmark
        GIT_REPOSITORY https://github.com/google/benchmark.git
        GIT_TAG        v1.8.3
    )
    set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
    set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(benchmark)

    add_subdirectory(benchmarks)
endif()

# ============================================================================
# Install Rules
# ============================================================================
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS hotcoco
    EXPORT hotcocoTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(DIRECTORY include/hotcoco
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(EXPORT hotcocoTargets
    FILE hotcocoTargets.cmake
    NAMESPACE hotcoco::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hotcoco
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/hotcocoConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/hotcocoConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hotcoco
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/hotcocoConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/hotcocoConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/hotcocoConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/hotcoco
)
