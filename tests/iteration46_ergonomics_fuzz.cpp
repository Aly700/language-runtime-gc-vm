#include "fuzz_common.hpp"

#include "lang/frontend/type_checker.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kFirstSeed = 46;
constexpr std::uint64_t kCorpusSize = 32;
constexpr std::uint64_t kSnapshotSeed = 46;
constexpr std::size_t kMutantCount = 18;

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

std::string diagnostics_listing(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.position.line << ":"
            << diagnostic.position.column << " "
            << diagnostic.message << "\n";
    }
    return out.str();
}

std::string generate_source(std::uint64_t seed) {
    fuzz::SplitMix64 random(seed ^ 0x46E2'7A9B'51C8'03D1ull);
    const auto number =
        static_cast<std::int64_t>(100 + random.bounded(900));
    const auto magnitude =
        static_cast<std::int64_t>(1 + random.bounded(40));
    const auto range_limit =
        static_cast<std::int64_t>(2 + random.bounded(4));
    const auto first_value =
        static_cast<std::int64_t>(1 + random.bounded(20));
    const auto second_value =
        static_cast<std::int64_t>(21 + random.bounded(20));
    const auto token = std::to_string(number);
    const auto needle =
        token.substr(static_cast<std::size_t>(
                         random.bounded(token.size())),
                     1);
    const auto haystack = std::string("pre-") + token + "-suffix";

    std::ostringstream out;
    out << "let haystack: str = \"" << haystack << "\";\n"
        << "let needle: str = \"" << needle << "\";\n"
        << "let texts: [str] = [haystack, \"no-match-" << seed
        << "\"];\n"
        << "let entries: map<str, i64> = map<str, i64>();\n"
        << "entries[\"pre-first\"] = " << first_value << ";\n"
        << "entries[\"other\"] = " << second_value << ";\n"
        << "let range_hits: i64 = 0;\n"
        << "range_outer: for i in 0.." << range_limit << " {\n"
        << "  range_inner: while true {\n"
        << "    range_hits = range_hits + 1;\n"
        << "    continue range_outer;\n"
        << "  }\n"
        << "}\n"
        << "let array_hits: i64 = 0;\n"
        << "array_outer: for text in texts {\n"
        << "  array_middle: for j in 0..3 {\n"
        << "    array_inner: while true {\n"
        << "      array_hits = array_hits + 1;\n"
        << "      if text.contains(needle) {\n"
        << "        continue array_outer;\n"
        << "      } else {\n"
        << "        break array_middle;\n"
        << "      }\n"
        << "    }\n"
        << "  }\n"
        << "}\n"
        << "let map_total: i64 = 0;\n"
        << "map_outer: for key, value in entries {\n"
        << "  map_inner: while true {\n"
        << "    map_total = map_total + max(value, 0);\n"
        << "    if key.starts_with(\"pre\") {\n"
        << "      continue map_outer;\n"
        << "    } else {\n"
        << "      break map_outer;\n"
        << "    }\n"
        << "  }\n"
        << "}\n"
        << "let magnitude: i64 = abs(-" << magnitude << ");\n"
        << "let low: i64 = min(" << first_value << ", "
        << second_value << ");\n"
        << "let high: i64 = max(" << first_value << ", "
        << second_value << ");\n"
        << "let contained: bool = haystack.contains(needle);\n"
        << "let found: i64 = haystack.index_of(needle);\n"
        << "let absent: i64 = haystack.index_of(\"not-present\");\n"
        << "let prefixed: bool = haystack.starts_with(\"pre-\");\n"
        << "let suffixed: bool = haystack.ends_with(\"-suffix\");\n"
        << "let score: i64 = range_hits + array_hits + map_total + "
           "magnitude + low + high + found + absent;\n"
        << "print(to_str(score));\n"
        << "print(to_str(contained));\n"
        << "print(to_str(prefixed));\n"
        << "print(to_str(suffixed));\n"
        << "print(to_str(absent));\n"
        << "pair(haystack, pair(needle, pair(to_str(score), "
           "pair(to_str(contained), to_str(suffixed)))))\n";
    return out.str();
}

std::string corpus_dump() {
    std::ostringstream out;
    for (std::uint64_t seed = kFirstSeed;
         seed < kFirstSeed + kCorpusSize; ++seed) {
        out << "===== grammar ergonomics seed " << seed << " =====\n"
            << generate_source(seed);
    }
    return out.str();
}

std::string replay_command(std::uint64_t seed,
                           const fuzz::Schedule& schedule) {
    return "./build/lang_iteration46_ergonomics_fuzz "
           "--grammar ergonomics --seed " +
           std::to_string(seed) + " --schedule " + schedule.name;
}

lang::VerifiedModule compile_source(std::uint64_t seed,
                                    const std::string& source) {
    const auto compiled = lang::frontend::compile_program(source);
    require(
        compiled.ok() && compiled.verified_module.has_value(),
        "ergonomics grammar rejected seed=" + std::to_string(seed) +
            "\nrepro: " +
            replay_command(
                seed,
                fuzz::find_schedule(fuzz::schedules(), "no_stress")) +
            "\nsource:\n" + source + "\ndiagnostics:\n" +
            diagnostics_listing(compiled.diagnostics));
    const auto& verified = *compiled.verified_module;
    const auto report =
        lang::verify_with_diagnostics(verified.module());
    require(report.result.has_value(),
            "ergonomics grammar violated compiler/verifier agreement seed=" +
                std::to_string(seed));

    std::size_t abs_count = 0;
    std::size_t substring_count = 0;
    for (const auto& function : verified.module().functions) {
        abs_count += static_cast<std::size_t>(std::count_if(
            function.code.begin(), function.code.end(),
            [](const lang::Instruction& instruction) {
                return instruction.op == lang::OpCode::I64Abs;
            }));
        substring_count += static_cast<std::size_t>(std::count_if(
            function.code.begin(), function.code.end(),
            [](const lang::Instruction& instruction) {
                return instruction.op == lang::OpCode::StrSub;
            }));
    }
    require(abs_count == 1 && substring_count == 7,
            "ergonomics grammar omitted builtin lowering evidence seed=" +
                std::to_string(seed) + " abs=" +
                std::to_string(abs_count) + " substrings=" +
                std::to_string(substring_count));
    return verified;
}

void compare_schedule(std::uint64_t seed, const std::string& source,
                      const lang::VerifiedModule& module,
                      const fuzz::Outcome& baseline,
                      const fuzz::Schedule& schedule) {
    const auto observed =
        std::string_view(schedule.name) == "no_stress"
            ? baseline
            : fuzz::execute_once(module, schedule);
    require(
        baseline.ok && observed.ok,
        "ergonomics grammar trapped seed=" + std::to_string(seed) +
            " schedule=" + schedule.name + " baseline=" +
            baseline.error + " observed=" + observed.error +
            "\nrepro: " + replay_command(seed, schedule) +
            "\nsource:\n" + source);
    require(
        baseline.observable.starts_with("object(@0)") &&
            !baseline.output.empty(),
        "ergonomics grammar skipped graph or output oracle seed=" +
            std::to_string(seed));
    require(
        fuzz::same_observables(baseline, observed),
        "ergonomics observables drifted seed=" + std::to_string(seed) +
            " schedule=" + schedule.name +
            "\nrepro: " + replay_command(seed, schedule) +
            "\nsource:\n" + source + "\nbaseline graph:\n" +
            baseline.observable + "\nobserved graph:\n" +
            observed.observable + "\nbaseline output:\n" +
            fuzz::render_output_bytes(baseline.output) +
            "\nobserved output:\n" +
            fuzz::render_output_bytes(observed.output));
}

struct Mutant {
    std::string source;
    std::string expected;
};

Mutant mutant_source(std::uint64_t seed, std::size_t mutant) {
    const auto value = std::to_string(seed % 17 + 1);
    switch (mutant) {
    case 0:
        return {"while true { break missing; }\n0\n",
                "unknown loop label 'missing'"};
    case 1:
        return {"done: while false { }\n"
                "while true { continue done; }\n0\n",
                "does not lexically enclose this continue"};
    case 2:
        return {"outer: while true {\n"
                "  outer: for i in 0..1 { break; }\n"
                "  break;\n}\n0\n",
                "duplicates an active loop label"};
    case 3:
        return {"break outside;\n0\n",
                "unknown loop label 'outside'"};
    case 4:
        return {"abs()\n", "abs expects exactly 1 argument"};
    case 5:
        return {"abs(true)\n", "abs argument expects i64"};
    case 6:
        return {"min(" + value + ")\n",
                "min expects exactly 2 arguments"};
    case 7:
        return {"min(false, " + value + ")\n",
                "min argument expects i64"};
    case 8:
        return {"max(" + value + ")\n",
                "max expects exactly 2 arguments"};
    case 9:
        return {"max(" + value + ", \"x\")\n",
                "max argument expects i64"};
    case 10:
        return {"\"x\".contains()\n",
                "contains expects exactly 1 argument"};
    case 11:
        return {"\"x\".contains(" + value + ")\n",
                "contains argument expects str"};
    case 12:
        return {"\"x\".index_of(\"x\", \"y\")\n",
                "index_of expects exactly 1 argument"};
    case 13:
        return {value + ".index_of(\"x\")\n",
                "index_of requires str receiver"};
    case 14:
        return {"\"x\".starts_with()\n",
                "starts_with expects exactly 1 argument"};
    case 15:
        return {"\"x\".starts_with(false)\n",
                "starts_with argument expects str"};
    case 16:
        return {"\"x\".ends_with(\"x\", \"y\")\n",
                "ends_with expects exactly 1 argument"};
    case 17:
        return {value + ".ends_with(\"x\")\n",
                "ends_with requires str receiver"};
    }
    throw std::out_of_range("ergonomics mutant index");
}

void require_mutant_rejected(std::uint64_t seed,
                             std::size_t mutant) {
    const auto generated = mutant_source(seed, mutant);
    const auto compiled =
        lang::frontend::compile_program(generated.source);
    require(
        !compiled.ok() && !compiled.diagnostics.empty(),
        "ergonomics mutant unexpectedly compiled seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant) + "\nsource:\n" +
            generated.source);
    const auto stable = std::find_if(
        compiled.diagnostics.begin(), compiled.diagnostics.end(),
        [&](const lang::frontend::Diagnostic& diagnostic) {
            return diagnostic.message.find(generated.expected) !=
                   std::string::npos;
        });
    require(
        stable != compiled.diagnostics.end(),
        "ergonomics mutant omitted stable diagnostic seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant) + " expected=" +
            generated.expected + "\ndiagnostics:\n" +
            diagnostics_listing(compiled.diagnostics));
    require(
        std::all_of(
            compiled.diagnostics.begin(), compiled.diagnostics.end(),
            [&](const lang::frontend::Diagnostic& diagnostic) {
                return diagnostic.position.line > 0 &&
                       diagnostic.position.column > 0 &&
                       diagnostic.position.offset <
                           generated.source.size();
            }),
        "ergonomics mutant rejection lacked positioned diagnostics seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant));
}

void require_grammar(std::string_view grammar) {
    require(grammar == "ergonomics", "expected ergonomics grammar");
}

std::size_t parse_mutant(std::string_view text) {
    const auto parsed = fuzz::parse_seed(std::string(text));
    require(parsed < kMutantCount,
            "ergonomics mutant index out of range");
    return static_cast<std::size_t>(parsed);
}

void pinned_artifacts(
    const std::vector<fuzz::Schedule>& schedules) {
    const auto source = generate_source(kSnapshotSeed);
    const auto module = compile_source(kSnapshotSeed, source);
    const auto outcome = fuzz::execute_once(
        module, fuzz::find_schedule(schedules, "no_stress"));
    require(outcome.ok,
            "ergonomics representative source trapped: " +
                outcome.error);
    const auto combined =
        outcome.observable + "\nOUTPUT\n" + outcome.output;
    const auto corpus = corpus_dump();

    constexpr std::uint64_t kExpectedSourceHash =
        3'513'356'585'459'432'607ull;
    constexpr std::uint64_t kExpectedOutcomeHash =
        11'394'261'262'610'471'186ull;
    constexpr std::uint64_t kExpectedCorpusHash =
        7'085'578'191'262'596'976ull;
    const auto source_hash = fnv1a64(source);
    const auto outcome_hash = fnv1a64(combined);
    const auto corpus_hash = fnv1a64(corpus);
    require(
        source_hash == kExpectedSourceHash &&
            outcome_hash == kExpectedOutcomeHash &&
            corpus_hash == kExpectedCorpusHash,
        "ergonomics pinned artifacts changed: source=" +
            std::to_string(source_hash) + " outcome=" +
            std::to_string(outcome_hash) + " corpus=" +
            std::to_string(corpus_hash));
}

int run(int argc, char** argv) {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "ergonomics fuzz requires fifteen schedules");
    const auto& baseline_schedule =
        fuzz::find_schedule(schedules, "no_stress");

    if (argc == 7 &&
        (std::string_view(argv[1]) == "--grammar" ||
         std::string_view(argv[1]) == "--replay") &&
        std::string_view(argv[3]) == "--seed" &&
        std::string_view(argv[5]) == "--schedule") {
        require_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto source = generate_source(seed);
        const auto module = compile_source(seed, source);
        const auto baseline =
            fuzz::execute_once(module, baseline_schedule);
        const auto& schedule =
            fuzz::find_schedule(schedules, argv[6]);
        compare_schedule(
            seed, source, module, baseline, schedule);
        std::cerr << "[PASS] ergonomics replay seed=" << seed
                  << " schedule=" << schedule.name << "\n";
        return 0;
    }
    if (argc == 7 &&
        (std::string_view(argv[1]) == "--grammar" ||
         std::string_view(argv[1]) == "--replay") &&
        std::string_view(argv[3]) == "--seed" &&
        std::string_view(argv[5]) == "--mutant") {
        require_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto mutant = parse_mutant(argv[6]);
        require_mutant_rejected(seed, mutant);
        std::cerr << "[PASS] ergonomics mutant replay seed="
                  << seed << " mutant=" << mutant << "\n";
        return 0;
    }
    if (argc == 3 &&
        std::string_view(argv[1]) == "--dump-corpus") {
        require_grammar(argv[2]);
        std::cout << corpus_dump();
        return 0;
    }
    if (argc != 1) {
        std::cerr
            << "usage: " << argv[0]
            << " <--grammar|--replay> ergonomics --seed N "
               "--schedule NAME\n"
            << "       " << argv[0]
            << " <--grammar|--replay> ergonomics --seed N "
               "--mutant <0..17>\n"
            << "       " << argv[0]
            << " --dump-corpus ergonomics\n";
        return 2;
    }

    pinned_artifacts(schedules);
    for (std::uint64_t seed = kFirstSeed;
         seed < kFirstSeed + kCorpusSize; ++seed) {
        const auto source = generate_source(seed);
        const auto module = compile_source(seed, source);
        const auto baseline =
            fuzz::execute_once(module, baseline_schedule);
        for (const auto& schedule : schedules) {
            compare_schedule(
                seed, source, module, baseline, schedule);
        }
        for (std::size_t mutant = 0; mutant < kMutantCount;
             ++mutant) {
            require_mutant_rejected(seed, mutant);
        }
    }
    std::cerr
        << "[PASS] ergonomics_pinned_seed_snapshot seed="
        << kSnapshotSeed << "\n"
        << "[PASS] lang_iteration46_ergonomics_fuzz seeds="
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
        std::cerr << "[FAIL] iteration46 ergonomics fuzz: "
                  << error.what() << "\n";
        return 1;
    }
}
