#include "fuzz_common.hpp"

#include "lang/frontend/type_checker.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t kFirstSeed = 1;
constexpr std::uint64_t kCorpusSize = 32;
constexpr std::uint64_t kSnapshotSeed = 39;
constexpr std::size_t kMutantCount = 4;
// `--dump-corpus tailcalls` SHA-256:
// 72e0af127c314f7bfa4ceb3961170b452fc490e5ea9f31fcfb6643cddebeaf61

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::uint64_t fnv1a64(std::string_view text) {
    std::uint64_t hash = 0xcbf2'9ce4'8422'2325ull;
    for (const auto byte : text) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 0x0000'0100'0000'01b3ull;
    }
    return hash;
}

std::string generate_source(std::uint64_t seed) {
    fuzz::SplitMix64 random(seed ^ 0x39CA'11F0'B0A7'D4E5ull);
    const auto first = random.small_i64() + 100;
    const auto second = random.small_i64() + 200;
    const auto ping_delta =
        static_cast<std::int64_t>(random.bounded(5) + 1);
    const auto pong_delta =
        static_cast<std::int64_t>(random.bounded(7) + 1);
    const auto depth =
        static_cast<std::int64_t>(random.bounded(28) + 5);

    std::ostringstream out;
    out << "fn ping(n: i64, state: pair<i64, i64>) -> "
           "pair<i64, i64> {\n"
        << "  let stale: pair<i64, i64> = pair(n + 1000, n + 2000);\n"
        << "  if n < 1 {\n"
        << "  } else {\n"
        << "    state.left = state.left + " << ping_delta << ";\n"
        << "    return tail pong(n + -1, state);\n"
        << "  }\n"
        << "  state\n"
        << "}\n"
        << "fn pong(n: i64, state: pair<i64, i64>) -> "
           "pair<i64, i64> {\n"
        << "  let stale: pair<i64, i64> = pair(n + 3000, n + 4000);\n"
        << "  if n < 1 {\n"
        << "  } else {\n"
        << "    state.right = state.right + " << pong_delta << ";\n"
        << "    return tail ping(n + -1, state);\n"
        << "  }\n"
        << "  state\n"
        << "}\n"
        << "let state: pair<i64, i64> = pair(" << first << ", "
        << second << ");\n"
        << "let result: pair<i64, i64> = ping(" << depth
        << ", state);\n"
        << "print(to_str(result.left));\n"
        << "print(to_str(result.right));\n"
        << "result\n";
    return out.str();
}

std::string diagnostics(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.position.line << ":"
            << diagnostic.position.column << " "
            << diagnostic.message << "\n";
    }
    return out.str();
}

lang::VerifiedModule compile_source(std::uint64_t seed,
                                    const std::string& source) {
    const auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok() && compiled.verified_module.has_value(),
            "tailcalls grammar rejected seed=" + std::to_string(seed) +
                "\nsource:\n" + source + "\ndiagnostics:\n" +
                diagnostics(compiled.diagnostics));
    return *compiled.verified_module;
}

std::string replay_command(std::uint64_t seed,
                           const fuzz::Schedule& schedule) {
    return "./build/lang_iteration39_tail_calls_fuzz "
           "--grammar tailcalls --seed " +
           std::to_string(seed) + " --schedule " + schedule.name;
}

fuzz::Outcome compare_schedule(
    std::uint64_t seed, const std::string& source,
    const lang::VerifiedModule& module,
    const fuzz::Schedule& baseline_schedule,
    const fuzz::Schedule& schedule) {
    const auto baseline = fuzz::execute_once(module, baseline_schedule);
    const auto observed =
        std::string_view(schedule.name) == baseline_schedule.name
            ? baseline
            : fuzz::execute_once(module, schedule);
    require(
        baseline.ok && observed.ok,
        "tailcalls grammar trapped seed=" + std::to_string(seed) +
            " schedule=" + schedule.name +
            " baseline=" + baseline.error +
            " observed=" + observed.error + "\nrepro: " +
            replay_command(seed, schedule) + "\nsource:\n" + source);
    require(!baseline.observable.empty() && !baseline.output.empty(),
            "tailcalls grammar skipped graph or output oracle");
    require(
        fuzz::same_observables(baseline, observed),
        "tailcalls oracle drift seed=" + std::to_string(seed) +
            " schedule=" + schedule.name + "\nrepro: " +
            replay_command(seed, schedule) + "\nbaseline graph:\n" +
            baseline.observable + "\nobserved graph:\n" +
            observed.observable + "\nbaseline output bytes:\n" +
            fuzz::render_output_bytes(baseline.output) +
            "\nobserved output bytes:\n" +
            fuzz::render_output_bytes(observed.output));
    return observed;
}

std::string mutant_source(std::uint64_t seed, std::size_t mutant) {
    const auto value = static_cast<std::int64_t>(seed % 31);
    switch (mutant) {
    case 0:
        return "fn id(n: i64) -> i64 { n }\n"
               "return tail id(" +
               std::to_string(value) + ");\n0\n";
    case 1:
        return "fn bad() -> i64 {\n"
               "  return tail 1;\n"
               "  0\n"
               "}\n"
               "bad()\n";
    case 2:
        return "fn id(n: i64) -> i64 { n }\n"
               "fn bad(n: i64) -> i64 {\n"
               "  let callable: fn(i64) -> i64 = id;\n"
               "  return tail callable(n);\n"
               "  0\n"
               "}\n"
               "bad(" +
               std::to_string(value) + ")\n";
    case 3:
        return "fn flag() -> bool { true }\n"
               "fn bad() -> i64 {\n"
               "  return tail flag();\n"
               "  0\n"
               "}\n"
               "bad()\n";
    }
    throw std::out_of_range("tailcalls mutant index");
}

void require_mutant_rejected(std::uint64_t seed, std::size_t mutant) {
    const auto source = mutant_source(seed, mutant);
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok() && !compiled.diagnostics.empty(),
            "tailcalls mutant unexpectedly compiled seed=" +
                std::to_string(seed) + " mutant=" +
                std::to_string(mutant) + "\nsource:\n" + source);
    require(compiled.diagnostics.front().position.line != 0 &&
                compiled.diagnostics.front().position.column != 0,
            "tailcalls mutant diagnostic omitted source position");
}

void require_grammar(std::string_view grammar) {
    require(grammar == "tailcalls", "expected tailcalls grammar");
}

void pinned_incremental_outcomes(
    const std::vector<fuzz::Schedule>& schedules) {
    const auto source = generate_source(kSnapshotSeed);
    const auto module = compile_source(kSnapshotSeed, source);
    const auto& baseline = fuzz::find_schedule(schedules, "no_stress");

    constexpr std::uint64_t kExpectedSourceHash =
        7'023'477'066'099'164'481ull;
    require(fnv1a64(source) == kExpectedSourceHash,
            "tailcalls representative source pin changed: " +
                std::to_string(fnv1a64(source)));

    for (const auto name : {"incremental_1", "incremental_compact_1",
                            "combined_mark_compact"}) {
        const auto& schedule = fuzz::find_schedule(schedules, name);
        const auto outcome = compare_schedule(
            kSnapshotSeed, source, module, baseline, schedule);
        const auto combined =
            outcome.observable + "\nOUTPUT\n" + outcome.output;
        constexpr std::uint64_t kExpectedOutcomeHash =
            18'290'439'316'683'119'576ull;
        require(fnv1a64(combined) == kExpectedOutcomeHash,
                std::string("tailcalls outcome pin changed for ") + name +
                    ": " + std::to_string(fnv1a64(combined)));
    }
}

std::size_t parse_mutant(std::string_view text) {
    const auto parsed = fuzz::parse_seed(std::string(text));
    require(parsed < kMutantCount, "tailcalls mutant index out of range");
    return static_cast<std::size_t>(parsed);
}

int run(int argc, char** argv) {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "tailcalls fuzz requires fifteen schedules");
    const auto& baseline = fuzz::find_schedule(schedules, "no_stress");

    if (argc == 7 &&
        (std::string_view(argv[1]) == "--grammar" ||
         std::string_view(argv[1]) == "--replay") &&
        std::string_view(argv[3]) == "--seed" &&
        std::string_view(argv[5]) == "--schedule") {
        require_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto source = generate_source(seed);
        const auto module = compile_source(seed, source);
        const auto& schedule = fuzz::find_schedule(schedules, argv[6]);
        (void)compare_schedule(
            seed, source, module, baseline, schedule);
        std::cerr << "[PASS] tailcalls replay seed=" << seed
                  << " schedule=" << schedule.name << "\n";
        return 0;
    }
    if (argc == 7 &&
        (std::string_view(argv[1]) == "--grammar" ||
         std::string_view(argv[1]) == "--replay") &&
        std::string_view(argv[3]) == "--seed" &&
        std::string_view(argv[5]) == "--mutant") {
        require_grammar(argv[2]);
        require_mutant_rejected(
            fuzz::parse_seed(argv[4]), parse_mutant(argv[6]));
        return 0;
    }
    if (argc == 3 &&
        std::string_view(argv[1]) == "--dump-corpus") {
        require_grammar(argv[2]);
        for (std::uint64_t seed = kFirstSeed;
             seed < kFirstSeed + kCorpusSize; ++seed) {
            std::cout << "===== seed " << seed << " =====\n"
                      << generate_source(seed);
        }
        return 0;
    }
    if (argc != 1) {
        std::cerr
            << "usage: " << argv[0]
            << " <--grammar|--replay> tailcalls --seed N "
               "--schedule NAME\n"
            << "       " << argv[0]
            << " <--grammar|--replay> tailcalls --seed N "
               "--mutant <0..3>\n"
            << "       " << argv[0]
            << " --dump-corpus tailcalls\n";
        return 2;
    }

    pinned_incremental_outcomes(schedules);
    for (std::uint64_t seed = kFirstSeed;
         seed < kFirstSeed + kCorpusSize; ++seed) {
        const auto source = generate_source(seed);
        const auto module = compile_source(seed, source);
        for (const auto& schedule : schedules) {
            (void)compare_schedule(
                seed, source, module, baseline, schedule);
        }
        for (std::size_t mutant = 0; mutant < kMutantCount; ++mutant) {
            require_mutant_rejected(seed, mutant);
        }
    }
    std::cerr << "[PASS] lang_iteration39_tail_calls_fuzz seeds="
              << kCorpusSize << " schedules=" << schedules.size()
              << " executions=" << kCorpusSize * schedules.size()
              << " mutants=" << kCorpusSize * kMutantCount << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] iteration39 tailcalls fuzz: "
                  << error.what() << "\n";
        return 1;
    }
}
