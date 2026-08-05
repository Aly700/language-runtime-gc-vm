cmake_minimum_required(VERSION 3.20)
project(language_runtime_gc_vm LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_library(lang_runtime
    src/bytecode.cpp
    src/gc/heap.cpp
    src/trace.cpp
    src/vm.cpp
    src/frontend/compiler.cpp
    src/frontend/diagnostics.cpp
    src/frontend/generics.cpp
    src/frontend/lexer.cpp
    src/frontend/parser.cpp
    src/frontend/type_checker.cpp
    src/frontend/type_checker_internal.cpp
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

add_executable(lang_iteration7_calls tests/iteration7_calls.cpp)
target_link_libraries(lang_iteration7_calls PRIVATE lang_runtime)

add_executable(lang_frontend_pipeline tests/frontend_pipeline.cpp)
target_link_libraries(lang_frontend_pipeline PRIVATE lang_runtime)

add_executable(lang_iteration9_pair_types tests/iteration9_pair_types.cpp)
target_link_libraries(lang_iteration9_pair_types PRIVATE lang_runtime)

add_executable(lang_iteration10_source_fuzz tests/iteration10_source_fuzz.cpp)
target_link_libraries(lang_iteration10_source_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration12_handles tests/iteration12_handles.cpp)
target_link_libraries(lang_iteration12_handles PRIVATE lang_runtime)

add_executable(lang_iteration13_verifier_diagnostics tests/iteration13_verifier_diagnostics.cpp)
target_link_libraries(lang_iteration13_verifier_diagnostics PRIVATE lang_runtime)

add_executable(lang_iteration14_recursive_types tests/iteration14_recursive_types.cpp)
target_link_libraries(lang_iteration14_recursive_types PRIVATE lang_runtime)

add_executable(lang_iteration17_verified_module tests/iteration17_verified_module.cpp)
target_link_libraries(lang_iteration17_verified_module PRIVATE lang_runtime)

add_executable(lang_iteration20_arrays tests/iteration20_arrays.cpp)
target_link_libraries(lang_iteration20_arrays PRIVATE lang_runtime)

add_executable(lang_iteration21_ref_arrays tests/iteration21_ref_arrays.cpp)
target_link_libraries(lang_iteration21_ref_arrays PRIVATE lang_runtime)

add_executable(lang_iteration22_source_arrays tests/iteration22_source_arrays.cpp)
target_link_libraries(lang_iteration22_source_arrays PRIVATE lang_runtime)

add_executable(lang_iteration23_strings tests/iteration23_strings.cpp)
target_link_libraries(lang_iteration23_strings PRIVATE lang_runtime)

add_executable(lang_iteration24_closures tests/iteration24_closures.cpp)
target_link_libraries(lang_iteration24_closures PRIVATE lang_runtime)

add_executable(lang_iteration25_maps tests/iteration25_maps.cpp)
target_link_libraries(lang_iteration25_maps PRIVATE lang_runtime)

add_executable(lang_iteration26_weak_refs tests/iteration26_weak_refs.cpp)
target_link_libraries(lang_iteration26_weak_refs PRIVATE lang_runtime)

add_executable(lang_iteration26_weak_source_fuzz tests/iteration26_weak_source_fuzz.cpp)
target_link_libraries(lang_iteration26_weak_source_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration28_for_in tests/iteration28_for_in.cpp)
target_link_libraries(lang_iteration28_for_in PRIVATE lang_runtime)

add_executable(lang_iteration29_output tests/iteration29_output.cpp)
target_link_libraries(lang_iteration29_output PRIVATE lang_runtime)

add_executable(lang_iteration30_break_continue tests/iteration30_break_continue.cpp)
target_link_libraries(lang_iteration30_break_continue PRIVATE lang_runtime)

add_executable(lang_iteration31_strings2 tests/iteration31_strings2.cpp)
target_link_libraries(lang_iteration31_strings2 PRIVATE lang_runtime)

add_executable(lang_iteration33_records tests/iteration33_records.cpp)
target_link_libraries(lang_iteration33_records PRIVATE lang_runtime)

add_executable(lang_iteration33_records_fuzz tests/iteration33_records_fuzz.cpp)
target_link_libraries(lang_iteration33_records_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration34_variants tests/iteration34_variants.cpp)
target_link_libraries(lang_iteration34_variants PRIVATE lang_runtime)

add_executable(lang_iteration34_variants_fuzz tests/iteration34_variants_fuzz.cpp)
target_link_libraries(lang_iteration34_variants_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration35_exceptions tests/iteration35_exceptions.cpp)
target_link_libraries(lang_iteration35_exceptions PRIVATE lang_runtime)

add_executable(lang_iteration35_exceptions_fuzz tests/iteration35_exceptions_fuzz.cpp)
target_link_libraries(lang_iteration35_exceptions_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration36_ephemerons tests/iteration36_ephemerons.cpp)
target_link_libraries(lang_iteration36_ephemerons PRIVATE lang_runtime)

add_executable(lang_iteration36_ephemerons_fuzz tests/iteration36_ephemerons_fuzz.cpp)
target_link_libraries(lang_iteration36_ephemerons_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration37_incremental_marking tests/iteration37_incremental_marking.cpp)
target_link_libraries(lang_iteration37_incremental_marking PRIVATE lang_runtime)

add_executable(lang_iteration38_incremental_compaction
    tests/iteration38_incremental_compaction.cpp)
target_link_libraries(lang_iteration38_incremental_compaction PRIVATE lang_runtime)

add_executable(lang_iteration38_incremental_compaction_fuzz
    tests/iteration38_incremental_compaction_fuzz.cpp)
target_link_libraries(
    lang_iteration38_incremental_compaction_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration39_tail_calls
    tests/iteration39_tail_calls.cpp)
target_link_libraries(lang_iteration39_tail_calls PRIVATE lang_runtime)

add_executable(lang_iteration39_tail_calls_fuzz
    tests/iteration39_tail_calls_fuzz.cpp)
target_link_libraries(
    lang_iteration39_tail_calls_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration40_map_lookup
    tests/iteration40_map_lookup.cpp)
target_link_libraries(lang_iteration40_map_lookup PRIVATE lang_runtime)

add_executable(lang_iteration41_generics
    tests/iteration41_generics.cpp)
target_link_libraries(lang_iteration41_generics PRIVATE lang_runtime)

add_executable(lang_iteration41_generics_fuzz
    tests/iteration41_generics_fuzz.cpp)
target_link_libraries(
    lang_iteration41_generics_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration42_generic_types
    tests/iteration42_generic_types.cpp)
target_link_libraries(
    lang_iteration42_generic_types PRIVATE lang_runtime)

add_executable(lang_iteration42_generic_types_fuzz
    tests/iteration42_generic_types_fuzz.cpp)
target_link_libraries(
    lang_iteration42_generic_types_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration43_large_object_space
    tests/iteration43_large_object_space.cpp)
target_link_libraries(
    lang_iteration43_large_object_space PRIVATE lang_runtime)

add_executable(lang_iteration44_string_interning
    tests/iteration44_string_interning.cpp)
target_link_libraries(
    lang_iteration44_string_interning PRIVATE lang_runtime)

add_executable(lang_iteration44_string_interning_fuzz
    tests/iteration44_string_interning_fuzz.cpp)
target_link_libraries(
    lang_iteration44_string_interning_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration45_runtime_diagnostics
    tests/iteration45_runtime_diagnostics.cpp)
target_link_libraries(
    lang_iteration45_runtime_diagnostics PRIVATE lang_runtime)

add_executable(lang_iteration46_ergonomics
    tests/iteration46_ergonomics.cpp)
target_link_libraries(
    lang_iteration46_ergonomics PRIVATE lang_runtime)

add_executable(lang_iteration46_ergonomics_fuzz
    tests/iteration46_ergonomics_fuzz.cpp)
target_link_libraries(
    lang_iteration46_ergonomics_fuzz PRIVATE lang_runtime)

add_executable(lang_iteration47_string_builder
    tests/iteration47_string_builder.cpp)
target_link_libraries(
    lang_iteration47_string_builder PRIVATE lang_runtime)

add_executable(lang_iteration47_string_builder_fuzz
    tests/iteration47_string_builder_fuzz.cpp)
target_link_libraries(
    lang_iteration47_string_builder_fuzz PRIVATE lang_runtime)

add_executable(lang_examples tests/lang_examples.cpp)
target_link_libraries(lang_examples PRIVATE lang_runtime)
target_compile_definitions(
    lang_examples PRIVATE LANG_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")

add_executable(lang_bench src/bench.cpp)
target_link_libraries(lang_bench PRIVATE lang_runtime)

add_executable(lang_trace src/tools/lang_trace.cpp)
target_link_libraries(lang_trace PRIVATE lang_runtime)
target_compile_options(lang_trace PRIVATE -Wall -Wextra -Wpedantic)

add_executable(trace_determinism tests/trace_determinism.cpp)
target_link_libraries(trace_determinism PRIVATE lang_runtime)
target_compile_options(trace_determinism PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(
    trace_determinism PRIVATE LANG_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")

add_executable(trace_transparency tests/trace_transparency.cpp)
target_link_libraries(trace_transparency PRIVATE lang_runtime)
target_compile_options(trace_transparency PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(
    trace_transparency PRIVATE LANG_EXAMPLES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/examples")

add_executable(trace_writer_run_metrics tests/trace_writer_run_metrics.cpp)
target_link_libraries(trace_writer_run_metrics PRIVATE lang_runtime)
target_compile_options(trace_writer_run_metrics PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
add_test(NAME lang_smoke COMMAND lang_smoke)
add_test(NAME lang_phase1_correctness COMMAND lang_phase1_correctness)
add_test(NAME lang_iteration2_correctness COMMAND lang_iteration2_correctness)
add_test(NAME lang_iteration3_movement COMMAND lang_iteration3_movement)
add_test(NAME lang_iteration4_generational COMMAND lang_iteration4_generational)
add_test(NAME lang_iteration5_fuzz COMMAND lang_iteration5_fuzz)
add_test(NAME lang_iteration7_calls COMMAND lang_iteration7_calls)
add_test(NAME lang_frontend_pipeline COMMAND lang_frontend_pipeline)
add_test(NAME lang_iteration9_pair_types COMMAND lang_iteration9_pair_types)
add_test(NAME lang_iteration10_source_fuzz COMMAND lang_iteration10_source_fuzz)
add_test(NAME lang_iteration12_handles COMMAND lang_iteration12_handles)
add_test(NAME lang_iteration13_verifier_diagnostics COMMAND lang_iteration13_verifier_diagnostics)
add_test(NAME lang_iteration14_recursive_types COMMAND lang_iteration14_recursive_types)
add_test(NAME lang_iteration17_verified_module COMMAND lang_iteration17_verified_module)
add_test(NAME lang_iteration20_arrays COMMAND lang_iteration20_arrays)
add_test(NAME lang_iteration21_ref_arrays COMMAND lang_iteration21_ref_arrays)
add_test(NAME lang_iteration22_source_arrays COMMAND lang_iteration22_source_arrays)
add_test(NAME lang_iteration23_strings COMMAND lang_iteration23_strings)
add_test(NAME lang_iteration24_closures COMMAND lang_iteration24_closures)
add_test(NAME lang_iteration25_maps COMMAND lang_iteration25_maps)
add_test(NAME lang_iteration26_weak_refs COMMAND lang_iteration26_weak_refs)
add_test(NAME lang_iteration26_weak_source_fuzz COMMAND lang_iteration26_weak_source_fuzz)
add_test(NAME lang_iteration28_for_in COMMAND lang_iteration28_for_in)
add_test(NAME lang_iteration29_output COMMAND lang_iteration29_output)
add_test(NAME lang_iteration30_break_continue COMMAND lang_iteration30_break_continue)
add_test(NAME lang_iteration31_strings2 COMMAND lang_iteration31_strings2)
add_test(NAME lang_iteration33_records COMMAND lang_iteration33_records)
add_test(NAME lang_iteration33_records_fuzz COMMAND lang_iteration33_records_fuzz)
add_test(NAME lang_iteration34_variants COMMAND lang_iteration34_variants)
add_test(NAME lang_iteration34_variants_fuzz COMMAND lang_iteration34_variants_fuzz)
add_test(NAME lang_iteration35_exceptions COMMAND lang_iteration35_exceptions)
add_test(NAME lang_iteration35_exceptions_fuzz COMMAND lang_iteration35_exceptions_fuzz)
add_test(NAME lang_iteration36_ephemerons COMMAND lang_iteration36_ephemerons)
add_test(NAME lang_iteration36_ephemerons_fuzz COMMAND lang_iteration36_ephemerons_fuzz)
add_test(NAME lang_iteration37_incremental_marking COMMAND lang_iteration37_incremental_marking)
add_test(NAME lang_iteration38_incremental_compaction
    COMMAND lang_iteration38_incremental_compaction)
add_test(NAME lang_iteration38_incremental_compaction_fuzz
    COMMAND lang_iteration38_incremental_compaction_fuzz)
add_test(NAME lang_iteration39_tail_calls
    COMMAND lang_iteration39_tail_calls)
add_test(NAME lang_iteration39_tail_calls_fuzz
    COMMAND lang_iteration39_tail_calls_fuzz)
add_test(NAME lang_iteration40_map_lookup
    COMMAND lang_iteration40_map_lookup)
add_test(NAME lang_iteration41_generics
    COMMAND lang_iteration41_generics)
add_test(NAME lang_iteration41_generics_fuzz
    COMMAND lang_iteration41_generics_fuzz)
add_test(NAME lang_iteration42_generic_types
    COMMAND lang_iteration42_generic_types)
add_test(NAME lang_iteration42_generic_types_fuzz
    COMMAND lang_iteration42_generic_types_fuzz)
add_test(NAME lang_iteration43_large_object_space
    COMMAND lang_iteration43_large_object_space)
add_test(NAME lang_iteration44_string_interning
    COMMAND lang_iteration44_string_interning)
add_test(NAME lang_iteration44_string_interning_fuzz
    COMMAND lang_iteration44_string_interning_fuzz)
add_test(NAME lang_iteration45_runtime_diagnostics
    COMMAND lang_iteration45_runtime_diagnostics)
add_test(NAME lang_iteration46_ergonomics
    COMMAND lang_iteration46_ergonomics)
add_test(NAME lang_iteration46_ergonomics_fuzz
    COMMAND lang_iteration46_ergonomics_fuzz)
add_test(NAME lang_iteration47_string_builder
    COMMAND lang_iteration47_string_builder)
add_test(NAME lang_iteration47_string_builder_fuzz
    COMMAND lang_iteration47_string_builder_fuzz)
add_test(NAME lang_examples COMMAND lang_examples)
add_test(NAME lang_bench_smoke COMMAND lang_bench --smoke --repetitions 1)
add_test(NAME trace_determinism COMMAND trace_determinism)
add_test(NAME trace_transparency COMMAND trace_transparency)
add_test(NAME trace_writer_run_metrics COMMAND trace_writer_run_metrics)

if(BUILD_TESTING)
find_program(
    LANG_TRACE_PYTHON3_EXECUTABLE
    NAMES python3
    PATHS ENV PATH
    NO_DEFAULT_PATH)
if(NOT LANG_TRACE_PYTHON3_EXECUTABLE)
    message(FATAL_ERROR
        "python3 is required for trace conservation tests; add python3 to PATH")
endif()
execute_process(
    COMMAND "${LANG_TRACE_PYTHON3_EXECUTABLE}" -c
        "import sys; raise SystemExit(0 if sys.version_info >= (3, 9) else 1)"
    RESULT_VARIABLE LANG_TRACE_PYTHON_VERSION_OK
    OUTPUT_QUIET
    ERROR_QUIET)
if(NOT LANG_TRACE_PYTHON_VERSION_OK EQUAL 0)
    message(FATAL_ERROR
        "Python 3.9 or newer is required for trace conservation and showcase tests")
endif()

set(
    TRACE_CONSERVATION_CHECKER
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/trace_conservation_check.py")
set(
    TRACE_CONSERVATION_SELFTEST
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/trace_conservation_selftest.py")
set(
    TRACE_CONSERVATION_ARTIFACT_ROOT
    "${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/trace_conservation")
set(TRACE_CONSERVATION_SNAPSHOT_INTERVAL 64)

function(add_trace_conservation_case case_name source_path schedule_name)
    set(
        bundle_directory
        "${TRACE_CONSERVATION_ARTIFACT_ROOT}/$<CONFIG>/${case_name}")
    set(fixture_name "trace_conservation_fixture_${case_name}")
    set(generate_test "trace_conservation_generate_${case_name}")
    set(check_test "trace_conservation_check_${case_name}")

    add_test(
        NAME "${generate_test}"
        COMMAND lang_trace
            "${CMAKE_CURRENT_SOURCE_DIR}/${source_path}"
            --schedule "${schedule_name}"
            --out "${bundle_directory}"
            --snapshot-interval "${TRACE_CONSERVATION_SNAPSHOT_INTERVAL}")
    set_tests_properties(
        "${generate_test}"
        PROPERTIES FIXTURES_SETUP "${fixture_name}")

    add_test(
        NAME "${check_test}"
        COMMAND "${LANG_TRACE_PYTHON3_EXECUTABLE}"
            "${TRACE_CONSERVATION_CHECKER}"
            "${bundle_directory}")
    set_tests_properties(
        "${check_test}"
        PROPERTIES
            FIXTURES_REQUIRED "${fixture_name}"
            PASS_REGULAR_EXPRESSION "OK events=.*snapshots_verified=")
endfunction()

add_trace_conservation_case(
    string_builder_combined_mark_compact
    examples/string_builder.lang
    combined_mark_compact)
add_trace_conservation_case(
    string_builder_incremental_compact_1
    examples/string_builder.lang
    incremental_compact_1)
add_trace_conservation_case(
    records_showcase_combined_mark_compact
    examples/records_showcase.lang
    combined_mark_compact)
add_trace_conservation_case(
    records_showcase_incremental_compact_1
    examples/records_showcase.lang
    incremental_compact_1)
add_trace_conservation_case(
    string_interning_combined_mark_compact
    examples/string_interning.lang
    combined_mark_compact)
add_trace_conservation_case(
    dead_owner_major_every_1
    tests/trace_inputs/dead_owner_before_forward.lang
    major_every_1)
add_trace_conservation_case(
    ephemeron_clear_major_every_1
    tests/trace_inputs/ephemeron_clear_before_forward.lang
    major_every_1)
add_trace_conservation_case(
    forward_then_update_incremental_1
    tests/trace_inputs/forward_then_update.lang
    incremental_1)

add_test(
    NAME trace_conservation_selftest
    COMMAND "${LANG_TRACE_PYTHON3_EXECUTABLE}"
        "${TRACE_CONSERVATION_SELFTEST}"
        "${TRACE_CONSERVATION_ARTIFACT_ROOT}/$<CONFIG>/string_builder_combined_mark_compact"
        "${TRACE_CONSERVATION_ARTIFACT_ROOT}/$<CONFIG>/records_showcase_combined_mark_compact"
        "${TRACE_CONSERVATION_ARTIFACT_ROOT}/$<CONFIG>/string_interning_combined_mark_compact"
        "${TRACE_CONSERVATION_ARTIFACT_ROOT}/$<CONFIG>/forward_then_update_incremental_1")
set_tests_properties(
    trace_conservation_selftest
    PROPERTIES
        FIXTURES_REQUIRED
            "trace_conservation_fixture_string_builder_combined_mark_compact;trace_conservation_fixture_records_showcase_combined_mark_compact;trace_conservation_fixture_string_interning_combined_mark_compact;trace_conservation_fixture_forward_then_update_incremental_1"
        PASS_REGULAR_EXPRESSION "SELFTEST OK corruptions=31")

set(
    SHOWCASE_PIN_OUTPUT
    "${CMAKE_CURRENT_BINARY_DIR}/Testing/Temporary/showcase_pin/$<CONFIG>")
add_test(
    NAME showcase_builder_selftest
    COMMAND "${LANG_TRACE_PYTHON3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_showcase_selftest.py")
set_tests_properties(
    showcase_builder_selftest
    PROPERTIES
        PASS_REGULAR_EXPRESSION "SELFTEST OK cases=10"
        TIMEOUT 30)
add_test(
    NAME showcase_pin
    COMMAND "${LANG_TRACE_PYTHON3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_showcase.py"
        --lang-trace "$<TARGET_FILE:lang_trace>"
        --output "${SHOWCASE_PIN_OUTPUT}"
        --verify-stable
        --compare-to "${CMAKE_CURRENT_SOURCE_DIR}/showcase")
set_tests_properties(
    showcase_pin
    PROPERTIES
        PASS_REGULAR_EXPRESSION "PIN OK files=[0-9]+ bytes=[0-9]+"
        RUN_SERIAL TRUE
        TIMEOUT 300)
endif()
