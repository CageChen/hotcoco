add_executable(hotcoco_bench
    core_bench.cpp
    sync_bench.cpp
    file_io_bench.cpp
    tcp_bench.cpp
)

target_link_libraries(hotcoco_bench
    PRIVATE
        hotcoco
        benchmark::benchmark
        benchmark::benchmark_main
)

target_compile_options(hotcoco_bench
    PRIVATE
        -O3
        -DNDEBUG
)
