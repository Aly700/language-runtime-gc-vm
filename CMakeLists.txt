cmake_minimum_required(VERSION 3.20)
project(language_runtime_gc_vm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(lang_runtime
    src/bytecode.cpp
    src/gc/heap.cpp
    src/vm.cpp
    src/frontend/type_checker.cpp
)
target_include_directories(lang_runtime PUBLIC include)
target_compile_options(lang_runtime PRIVATE -Wall -Wextra -Wpedantic)

add_executable(lang_smoke tests/smoke.cpp)
target_link_libraries(lang_smoke PRIVATE lang_runtime)

include(CTest)
add_test(NAME lang_smoke COMMAND lang_smoke)
