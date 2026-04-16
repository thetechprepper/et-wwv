cmake_minimum_required(VERSION 3.10)
project(et-wwv C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(et-wwv src/main.c)
target_link_libraries(et-wwv PRIVATE m asound)
