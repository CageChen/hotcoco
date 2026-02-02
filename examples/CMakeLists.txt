# Examples CMakeLists.txt

add_executable(01_basic_task
    01_basic_task.cpp
)

target_link_libraries(01_basic_task
    PRIVATE
        hotcoco
)

add_executable(02_generator
    02_generator.cpp
)

target_link_libraries(02_generator
    PRIVATE
        hotcoco
)

add_executable(03_async_sleep
    03_async_sleep.cpp
)

target_link_libraries(03_async_sleep
    PRIVATE
        hotcoco
)

add_executable(04_tcp_echo_server
    04_tcp_echo_server.cpp
)

target_link_libraries(04_tcp_echo_server
    PRIVATE
        hotcoco
)

add_executable(05_http_server
    05_http_server.cpp
)

target_link_libraries(05_http_server
    PRIVATE
        hotcoco
)
