#include "fuzz_common.hpp"

#include "lang/frontend/type_checker.hpp"
#include "lang/trace.hpp"
#include "lang/vm.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Fixture {
    std::string name;
    lang::VerifiedModule module;
};

struct Observation {
    bool succeeded{false};
    std::string error;
    std::string output;
    std::string canonical_result;
    std::optional<lang::RuntimeTrace> runtime_trace;
    lang::VMMetrics metrics;
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "could not open transparency fixture: " + path.string());
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
}

Fixture compile_fixture(std::string name, std::string source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        throw std::runtime_error(
            "fixture " + name + " failed to compile: " +
            (compiled.diagnostics.empty()
                 ? std::string("no diagnostic")
                 : compiled.diagnostics.front().message));
    }
    return Fixture{std::move(name),
                   std::move(*compiled.verified_module)};
}

std::string example_source(std::string_view name) {
    return read_file(std::filesystem::path(LANG_EXAMPLES_DIR) /
                     (std::string(name) + ".lang"));
}

bool same_stress(const lang::gc::StressConfig& left,
                 const lang::gc::StressConfig& right) {
    return left.collect_before_every_allocation ==
               right.collect_before_every_allocation &&
           left.collect_after_every_allocation ==
               right.collect_after_every_allocation &&
           left.collect_minor_after_every_write_barrier ==
               right.collect_minor_after_every_write_barrier &&
           left.collect_every_n_instructions ==
               right.collect_every_n_instructions &&
           left.collect_minor_every_n_instructions ==
               right.collect_minor_every_n_instructions &&
           left.incremental_mark_step_budgets ==
               right.incremental_mark_step_budgets &&
           left.incremental_compact_step_budgets ==
               right.incremental_compact_step_budgets;
}

Observation execute_fixture(
    const Fixture& fixture, const fuzz::Schedule& schedule,
    const std::filesystem::path* trace_directory) {
    std::unique_ptr<lang::JsonlTraceWriter> writer;
    if (trace_directory != nullptr) {
        writer = std::make_unique<lang::JsonlTraceWriter>(
            *trace_directory, 257);
    }

    lang::VM vm(writer.get());
    vm.set_gc_stress(schedule.stress);
    Observation observation;
    std::optional<lang::Value> result;
    try {
        result = vm.execute(fixture.module);
        observation.succeeded = true;
    } catch (const std::exception& error) {
        observation.error = error.what();
    }
    observation.output = fuzz::output_for(vm);
    observation.runtime_trace = vm.last_trap_trace();

    // Canonical-oracle validation is an embedder-side post-run action. Detach
    // the run sink so it cannot append events after the mandatory exit snapshot.
    vm.set_trace_sink(nullptr);
    if (result.has_value()) {
        observation.canonical_result =
            fuzz::observable_for(vm, *result);
    }
    observation.metrics = vm.metrics();
    return observation;
}

void require_transparent(const Observation& baseline,
                         const Observation& traced,
                         std::string_view fixture,
                         std::string_view schedule) {
    const auto context = std::string(fixture) + " under " +
                         std::string(schedule);
    if (baseline.succeeded != traced.succeeded ||
        baseline.error != traced.error) {
        throw std::runtime_error(
            "sink changed success/trap outcome for " + context);
    }
    if (baseline.output != traced.output) {
        throw std::runtime_error(
            "sink changed VM output bytes for " + context);
    }
    if (baseline.canonical_result != traced.canonical_result) {
        throw std::runtime_error(
            "sink changed canonical result graph for " + context);
    }
    if (baseline.runtime_trace != traced.runtime_trace) {
        throw std::runtime_error(
            "sink changed deterministic runtime diagnostics for " +
            context);
    }
    if (!(baseline.metrics == traced.metrics)) {
        throw std::runtime_error(
            "sink changed VM/Heap metrics for " + context);
    }
}

std::vector<Fixture> fixtures() {
    std::vector<Fixture> result;
    // Together these existing programs cover pairs, scalar/reference arrays,
    // strings, closures, maps, nominal records/variants, interning and Builder.
    for (const auto name : {"generic_types_showcase",
                            "capture_accumulator", "word_frequency",
                            "string_interning", "string_builder"}) {
        result.push_back(
            compile_fixture(name, example_source(name)));
    }
    result.push_back(compile_fixture("weak_ephemeron", R"(
let target: pair<i64, i64> = pair(1, 2);
let held: weak<pair<i64, i64>> = weak(target);
let key: pair<i64, i64> = pair(3, 4);
let conditional: pair<i64, i64> = pair(5, 6);
let entry: ephemeron<pair<i64, i64>, pair<i64, i64>> =
    ephemeron(key, conditional);
entry.set_value(conditional);
pair(held, entry)
)"));
    result.push_back(compile_fixture("intern_eviction", R"(
let index: i64 = 0;
let transient: str = "seed";
while index < 6 {
  transient = intern("value-" + to_str(index));
  index = index + 1;
}
transient
)"));
    result.push_back(compile_fixture("typed_exception", R"(
variant Error { Bad(i64), Other(str) }
fn fail() -> i64 { throw Error.Bad(41); 0 }
let answer: i64 = 0;
try {
  answer = fail();
} catch (error: Error) {
  match error {
    Bad(code) => { answer = code + 1; },
    Other(text) => { print(text); }
  }
}
answer
)"));
    result.push_back(compile_fixture("uncaught_exception", R"(
variant Failure { Fatal(str) }
throw Failure.Fatal("boom");
0
)"));
    result.push_back(compile_fixture(
        "runtime_trap", example_source("diagnostics_showcase")));
    return result;
}

} // namespace

int main() {
    try {
        const auto all_schedules = fuzz::schedules();
        if (all_schedules.size() != 15 ||
            lang::trace_schedule_names().size() != 15) {
            throw std::runtime_error(
                "trace schedule catalog is not exactly the shared 15");
        }
        const auto& incremental_compact_mixed = fuzz::find_schedule(
            all_schedules, "incremental_compact_3_1");
        const auto& incremental_compact_stress =
            incremental_compact_mixed.stress;
        if (incremental_compact_stress.collect_before_every_allocation ||
            incremental_compact_stress.collect_after_every_allocation ||
            incremental_compact_stress.collect_minor_after_every_write_barrier ||
            incremental_compact_stress.collect_every_n_instructions != 0 ||
            incremental_compact_stress.collect_minor_every_n_instructions != 0 ||
            incremental_compact_stress.incremental_mark_step_budgets !=
                std::vector<std::size_t>{3, 1} ||
            incremental_compact_stress.incremental_compact_step_budgets !=
                std::vector<std::size_t>{3, 1}) {
            throw std::runtime_error(
                "incremental_compact_3_1 fuzz schedule contract drifted");
        }
        for (const auto& schedule : all_schedules) {
            lang::gc::StressConfig traced_schedule;
            if (!lang::configure_trace_schedule(schedule.name,
                                                traced_schedule) ||
                !same_stress(schedule.stress, traced_schedule)) {
                throw std::runtime_error(
                    std::string("trace CLI schedule drifted from fuzz schedule ") +
                    schedule.name);
            }
        }

        const auto programs = fixtures();
        const auto artifact_root =
            std::filesystem::current_path() /
            "trace_transparency_artifacts";
        for (const auto& fixture : programs) {
            for (const auto& schedule : all_schedules) {
                const auto baseline = execute_fixture(
                    fixture, schedule, nullptr);
                const auto trace_directory =
                    artifact_root / fixture.name / schedule.name;
                const auto traced = execute_fixture(
                    fixture, schedule, &trace_directory);
                require_transparent(baseline, traced, fixture.name,
                                    schedule.name);
                if (fixture.name == "intern_eviction" &&
                    std::string_view(schedule.name) == "major_every_1" &&
                    read_file(trace_directory / "events.jsonl").find(
                        "\"kind\":\"evict\"") == std::string::npos) {
                    throw std::runtime_error(
                        "intern eviction fixture emitted no evict event");
                }
            }
            std::cerr << "[PASS] transparent across 15 schedules: "
                      << fixture.name << "\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
