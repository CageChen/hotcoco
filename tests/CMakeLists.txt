# Tests CMakeLists.txt

add_executable(hotcoco_test
    cancellation_test.cpp
    channel_test.cpp
    combinator_test.cpp
    concepts_test.cpp
    defer_test.cpp
    detached_task_test.cpp
    error_test.cpp
    event_test.cpp
    execution_test.cpp
    executor_test.cpp
    frame_pool_test.cpp
    generator_test.cpp
    http_test.cpp
    invoke_test.cpp
    iouring_executor_test.cpp
    iouring_tcp_test.cpp
    latch_test.cpp
    mutex_test.cpp
    periodic_timer_test.cpp
    polling_executor_test.cpp
    result_test.cpp
    retry_test.cpp
    rwlock_test.cpp
    schedule_on_test.cpp
    semaphore_test.cpp
    spawn_test.cpp
    stress_test.cpp
    sync_tcp_test.cpp
    sync_wait_test.cpp
    task_group_test.cpp
    task_test.cpp
    tcp_test.cpp
    thread_pool_test.cpp
    thread_utils_test.cpp
    timeout_test.cpp
    timer_test.cpp
)

target_link_libraries(hotcoco_test
    PRIVATE
        hotcoco
        GTest::gtest_main
)

# GTest requires exceptions; override the library's -fno-exceptions for test code
target_compile_options(hotcoco_test PRIVATE -fexceptions)

include(GoogleTest)
gtest_discover_tests(hotcoco_test)
