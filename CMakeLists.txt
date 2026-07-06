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

add_executable(lang_phase1_correctness tests/phase1_correctness.cpp)
target_link_libraries(lang_phase1_correctness PRIVATE lang_runtime)

add_executable(lang_iteration2_correctness tests/iteration2_correctness.cpp)
target_link_libraries(lang_iteration2_correctness PRIVATE lang_runtime)

add_executable(lang_iteration3_movement tests/iteration3_movement.cpp)
target_link_libraries(lang_iteration3_movement PRIVATE lang_runtime)

add_executable(lang_iteration4_generational tests/iteration4_generational.cpp)
target_link_libraries(lang_iteration4_generational PRIVATE lang_runtime)

add_executable(lang_iteration5_fuzz tests/iteration5_fuzz.cpp)
target_link_libraries(lang_iteration5_fuzz PRIVATE lang_runtime)

add_executable(lang_frontend_pipeline tests/frontend_pipeline.cpp)
target_link_libraries(lang_frontend_pipeline PRIVATE lang_runtime)

include(CTest)
add_test(NAME lang_smoke COMMAND lang_smoke)
add_test(NAME lang_phase1_correctness COMMAND lang_phase1_correctness)
add_test(NAME lang_iteration2_correctness COMMAND lang_iteration2_correctness)
add_test(NAME lang_iteration3_movement COMMAND lang_iteration3_movement)
add_test(NAME lang_iteration4_generational COMMAND lang_iteration4_generational)
add_test(NAME lang_iteration5_fuzz COMMAND lang_iteration5_fuzz)
add_test(NAME lang_frontend_pipeline COMMAND lang_frontend_pipeline)
