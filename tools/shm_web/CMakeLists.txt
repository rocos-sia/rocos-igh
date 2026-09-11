cmake_minimum_required(VERSION 3.16)
project(shm_web LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(shm_web_server shm_web_server.cpp)

target_include_directories(shm_web_server PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
)

target_compile_options(shm_web_server PRIVATE -Wall -Wextra)

target_link_libraries(shm_web_server PRIVATE
    Threads::Threads
    rt
)

# 把前端资源复制到可执行文件旁，方便直接运行。
add_custom_target(shm_web_assets ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
        "$<TARGET_FILE_DIR:shm_web_server>/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/value_decoder.mjs"
        "$<TARGET_FILE_DIR:shm_web_server>/value_decoder.mjs"
    COMMENT "Copying frontend assets next to shm_web_server"
)

add_custom_command(TARGET shm_web_server POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/index.html"
        "$<TARGET_FILE_DIR:shm_web_server>/index.html"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/value_decoder.mjs"
        "$<TARGET_FILE_DIR:shm_web_server>/value_decoder.mjs"
)
