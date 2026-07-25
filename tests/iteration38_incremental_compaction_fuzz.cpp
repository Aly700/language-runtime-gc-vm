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
constexpr std::uint64_t kSnapshotSeed = 38;
constexpr std::size_t kMutantCount = 4;
// `--dump-corpus incremental_compaction` SHA-256:
// 7992ef70300c905f9f2147e7a1438d3d24a2441a91e6357d14b981b1032175de

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
    fuzz::SplitMix64 random(seed ^ 0x38C0'4FAC'710A'B1E5ull);
    const auto first = random.small_i64() + 80;
    const auto second = random.small_i64() + 120;
    const auto replacement = random.small_i64() + 160;
    const auto array_value = random.small_i64() + 200;
    const bool choose_first = random.bounded(2) == 0;
    const auto key = std::string("key-") + std::to_string(seed % 17);

    std::ostringstream out;
    out << "variant Choice { First(pair<i64, i64>, str), "
           "Second(str, pair<i64, i64>) }\n"
        << "record Bundle {\n"
        << "  primary: pair<i64, i64>,\n"
        << "  scalars: [i64],\n"
        << "  refs: [pair<i64, i64>],\n"
        << "  text: str,\n"
        << "  capture: fn() -> pair<i64, i64>,\n"
        << "  table: map<str, pair<i64, i64>>,\n"
        << "  choice: Choice,\n"
        << "  observer: weak<pair<i64, i64>>,\n"
        << "  entry: ephemeron<pair<i64, i64>, pair<i64, i64>>\n"
        << "}\n\n"
        << "fn capture(value: pair<i64, i64>) -> "
           "fn() -> pair<i64, i64> {\n"
        << "  fn() -> pair<i64, i64> { value }\n"
        << "}\n\n"
        << "let discarded: pair<i64, i64> = pair(-1, -2);\n"
        << "discarded = pair(-3, -4);\n"
        << "let primary: pair<i64, i64> = pair(" << first << ", "
        << second << ");\n"
        << "let peer: pair<i64, i64> = pair(" << second << ", "
        << replacement << ");\n"
        << "primary.right = " << replacement << ";\n"
        << "let scalars: [i64] = [" << first << ", " << second
        << ", " << replacement << "];\n"
        << "scalars[1] = " << array_value << ";\n"
        << "let refs: [pair<i64, i64>] = [primary, peer, discarded];\n"
        << "refs[2] = primary;\n"
        << "let text: str = \"seed-" << seed << ":\" + to_str("
        << array_value << ");\n"
        << "let close: fn() -> pair<i64, i64> = capture(primary);\n"
        << "let table: map<str, pair<i64, i64>> = "
           "map<str, pair<i64, i64>>();\n"
        << "table[\"" << key << "\"] = peer;\n"
        << "table[\"" << key << "\"] = primary;\n"
        << "let observer: weak<pair<i64, i64>> = weak(peer);\n"
        << "let entry: ephemeron<pair<i64, i64>, pair<i64, i64>> = "
           "ephemeron(primary, peer);\n"
        << "entry.set_value(primary);\n"
        << "let choice: Choice = ";
    if (choose_first) {
        out << "Choice.First(peer, text);\n";
    } else {
        out << "Choice.Second(text, peer);\n";
    }
    out << "let bundle: Bundle = Bundle { primary: primary, scalars: "
           "scalars, refs: refs, text: text, capture: close, table: table, "
           "choice: choice, observer: observer, entry: entry };\n"
        << "let captured: pair<i64, i64> = bundle.capture();\n"
        << "print(bundle.text);\n"
        << "print(to_str(bundle.scalars[1] + captured.left));\n"
        << "bundle\n";
    return out.str();
}

std::string diagnostics(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.position.line << ":" << diagnostic.position.column
            << " " << diagnostic.message << "\n";
    }
    return out.str();
}

lang::VerifiedModule compile_source(std::uint64_t seed,
                                    const std::string& source) {
    const auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok() && compiled.verified_module.has_value(),
            "incremental compaction grammar rejected seed=" +
                std::to_string(seed) + "\nsource:\n" + source +
                "\ndiagnostics:\n" + diagnostics(compiled.diagnostics));
    return *compiled.verified_module;
}

std::string replay_command(std::uint64_t seed,
                           const fuzz::Schedule& schedule) {
    return "./build/lang_iteration38_incremental_compaction_fuzz "
           "--grammar incremental_compaction --seed " +
           std::to_string(seed) + " --schedule " + schedule.name;
}

fuzz::Outcome compare_schedule(
    std::uint64_t seed, const std::string& source,
    const lang::VerifiedModule& module,
    const fuzz::Schedule& baseline_schedule,
    const fuzz::Schedule& schedule) {
    const auto baseline = fuzz::execute_once(module, baseline_schedule);
    const auto observed = std::string_view(schedule.name) ==
                                  baseline_schedule.name
                              ? baseline
                              : fuzz::execute_once(module, schedule);
    require(baseline.ok && observed.ok,
            "incremental compaction grammar trapped seed=" +
                std::to_string(seed) + " schedule=" + schedule.name +
                " baseline=" + baseline.error +
                " observed=" + observed.error + "\nrepro: " +
                replay_command(seed, schedule) + "\nsource:\n" + source);
    require(!baseline.observable.empty() && !baseline.output.empty(),
            "incremental compaction grammar skipped graph or output oracle");
    require(
        fuzz::same_observables(baseline, observed),
        "incremental compaction oracle drift seed=" +
            std::to_string(seed) + " schedule=" + schedule.name +
            "\nrepro: " + replay_command(seed, schedule) +
            "\nbaseline graph:\n" + baseline.observable +
            "\nobserved graph:\n" + observed.observable +
            "\nbaseline output bytes:\n" +
            fuzz::render_output_bytes(baseline.output) +
            "\nobserved output bytes:\n" +
            fuzz::render_output_bytes(observed.output));
    return observed;
}

std::string mutant_source(std::uint64_t seed, std::size_t mutant) {
    const auto value = static_cast<std::int64_t>(seed % 31);
    switch (mutant) {
    case 0:
        return "let p: pair<i64, i64> = pair(1, 2);\n"
               "let m: map<str, pair<i64, i64>> = "
               "map<str, pair<i64, i64>>();\n"
               "m[1] = p;\nm\n";
    case 1:
        return "let p: pair<i64, i64> = pair(1, 2);\n"
               "let e: ephemeron<pair<i64, i64>, i64> = "
               "ephemeron(1, 2);\ne\n";
    case 2:
        return "record R { value: i64, next: pair<i64, i64> }\n"
               "R { value: 1 }\n";
    case 3:
        return "variant V { A(i64), B(str) }\n"
               "let v: V = V.A(\"bad\");\n" +
               std::to_string(value) + "\n";
    }
    throw std::out_of_range("incremental compaction mutant index");
}

void require_mutant_rejected(std::uint64_t seed, std::size_t mutant) {
    const auto compiled =
        lang::frontend::compile_program(mutant_source(seed, mutant));
    require(!compiled.ok() && !compiled.diagnostics.empty(),
            "incremental compaction mutant unexpectedly compiled seed=" +
                std::to_string(seed) +
                " mutant=" + std::to_string(mutant));
}

void require_grammar(std::string_view grammar) {
    require(grammar == "incremental_compaction",
            "expected incremental_compaction grammar");
}

void pinned_new_schedule_outcomes(
    const std::vector<fuzz::Schedule>& schedules) {
    const auto source = generate_source(kSnapshotSeed);
    const auto module = compile_source(kSnapshotSeed, source);
    const auto& baseline = fuzz::find_schedule(schedules, "no_stress");
    constexpr std::uint64_t kExpectedSourceHash =
        10'989'429'606'424'161'695ull;
    require(fnv1a64(source) == kExpectedSourceHash,
            "incremental compaction representative source pin changed: " +
                std::to_string(fnv1a64(source)));

    for (const auto name : {"incremental_compact_1",
                            "incremental_compact_3_1",
                            "combined_mark_compact"}) {
        const auto& schedule = fuzz::find_schedule(schedules, name);
        const auto outcome =
            compare_schedule(kSnapshotSeed, source, module, baseline, schedule);
        const auto combined = outcome.observable + "\nOUTPUT\n" + outcome.output;
        constexpr std::uint64_t kExpectedOutcomeHash =
            17'449'916'394'975'722'024ull;
        require(fnv1a64(combined) == kExpectedOutcomeHash,
                std::string("incremental compaction outcome pin changed for ") +
                    name + ": " + std::to_string(fnv1a64(combined)));
    }
}

std::size_t parse_mutant(std::string_view text) {
    const auto parsed = fuzz::parse_seed(std::string(text));
    require(parsed < kMutantCount,
            "incremental compaction mutant index out of range");
    return static_cast<std::size_t>(parsed);
}

int run(int argc, char** argv) {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "incremental compaction fuzz requires fifteen schedules");
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
        std::cerr << "[PASS] incremental compaction replay seed=" << seed
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
    if (argc == 3 && std::string_view(argv[1]) == "--dump-corpus") {
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
            << " <--grammar|--replay> incremental_compaction --seed N "
               "--schedule NAME\n"
            << "       " << argv[0]
            << " <--grammar|--replay> incremental_compaction --seed N "
               "--mutant <0..3>\n"
            << "       " << argv[0]
            << " --dump-corpus incremental_compaction\n";
        return 2;
    }

    pinned_new_schedule_outcomes(schedules);
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
    std::cerr
        << "[PASS] lang_iteration38_incremental_compaction_fuzz seeds="
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
        std::cerr << "[FAIL] iteration38 incremental compaction fuzz: "
                  << error.what() << "\n";
        return 1;
    }
}
