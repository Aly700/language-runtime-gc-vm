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

constexpr std::uint64_t kFirstSeed = 47;
constexpr std::uint64_t kCorpusSize = 32;
constexpr std::uint64_t kSnapshotSeed = 47;
constexpr std::size_t kMutantCount = 8;

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
    fuzz::SplitMix64 random(seed ^ 0x47B0'11DE'5A17'B00Full);
    const auto number = random.small_i64();
    const auto key = static_cast<std::int64_t>(1 + random.bounded(9));
    const auto active = random.bounded(2) == 0 ? "false" : "true";
    const auto map_suffix = random.bounded(100);
    const auto capture_suffix = random.bounded(100);
    const auto scratch_suffix = random.bounded(100);

    std::ostringstream out;
    out << "record BuilderBundle {\n"
        << "  primary: builder,\n"
        << "  scratch: builder,\n"
        << "  table: map<i64, builder>,\n"
        << "  render: fn(str) -> str,\n"
        << "  observer: weak<builder>,\n"
        << "  first: str,\n"
        << "  last: str\n"
        << "}\n\n"
        << "fn extend(target: builder, suffix: str) -> str {\n"
        << "  target.append(suffix);\n"
        << "  target.to_str()\n"
        << "}\n\n"
        << "let primary: builder = builder();\n"
        << "primary.append(\"seed-" << seed << "-\");\n"
        << "primary.append(" << number << ");\n"
        << "primary.append(" << active << ");\n"
        << "let first: str = primary.to_str();\n"
        << "let table: map<i64, builder> = map<i64, builder>();\n"
        << "table[" << key << "] = primary;\n"
        << "let via_map: builder = table[" << key << "];\n"
        << "let mapped: str = extend(via_map, \"-map-" << map_suffix
        << "\");\n"
        << "let render: fn(str) -> str = fn(suffix: str) -> str {\n"
        << "  primary.append(suffix);\n"
        << "  primary.to_str()\n"
        << "};\n"
        << "let captured: str = render(\"-capture-" << capture_suffix
        << "\");\n"
        << "let scratch: builder = builder();\n"
        << "scratch.append(\"discard\");\n"
        << "scratch.clear();\n"
        << "scratch.append(\"reuse-" << scratch_suffix << "\");\n"
        << "let observer: weak<builder> = weak(primary);\n"
        << "print(first);\n"
        << "print(mapped);\n"
        << "print(captured);\n"
        << "print(scratch.to_str());\n"
        << "print(to_str(primary.len));\n"
        << "let last: str = primary.to_str();\n"
        << "let bundle: BuilderBundle = BuilderBundle { "
           "primary: primary, scratch: scratch, table: table, "
           "render: render, observer: observer, first: first, last: last };\n"
        << "bundle\n";
    return out.str();
}

std::string corpus_dump() {
    std::ostringstream out;
    for (std::uint64_t seed = kFirstSeed;
         seed < kFirstSeed + kCorpusSize; ++seed) {
        out << "===== grammar builder seed " << seed << " =====\n"
            << generate_source(seed);
    }
    return out.str();
}

std::string replay_command(std::uint64_t seed,
                           const fuzz::Schedule& schedule) {
    return "./build/lang_iteration47_string_builder_fuzz "
           "--grammar builder --seed " +
           std::to_string(seed) + " --schedule " + schedule.name;
}

lang::VerifiedModule compile_source(std::uint64_t seed,
                                    const std::string& source) {
    const auto compiled = lang::frontend::compile_program(source);
    require(
        compiled.ok() && compiled.verified_module.has_value(),
        "builder grammar rejected seed=" + std::to_string(seed) +
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
            "builder grammar violated compiler/verifier agreement seed=" +
                std::to_string(seed));

    std::size_t alloc_count = 0;
    std::size_t append_count = 0;
    std::size_t len_count = 0;
    std::size_t snapshot_count = 0;
    std::size_t clear_count = 0;
    for (const auto& function : verified.module().functions) {
        for (const auto& instruction : function.code) {
            switch (instruction.op) {
            case lang::OpCode::AllocBuilder:
                ++alloc_count;
                break;
            case lang::OpCode::BuilderAppend:
                ++append_count;
                break;
            case lang::OpCode::BuilderLen:
                ++len_count;
                break;
            case lang::OpCode::BuilderToStr:
                ++snapshot_count;
                break;
            case lang::OpCode::BuilderClear:
                ++clear_count;
                break;
            default:
                break;
            }
        }
    }
    require(alloc_count == 2 && append_count == 7 &&
                len_count == 1 && snapshot_count == 5 &&
                clear_count == 1,
            "builder grammar omitted opcode coverage evidence seed=" +
                std::to_string(seed) + " alloc=" +
                std::to_string(alloc_count) + " append=" +
                std::to_string(append_count) + " len=" +
                std::to_string(len_count) + " snapshots=" +
                std::to_string(snapshot_count) + " clear=" +
                std::to_string(clear_count));
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
        "builder grammar trapped seed=" + std::to_string(seed) +
            " schedule=" + schedule.name + " baseline=" +
            baseline.error + " observed=" + observed.error +
            "\nrepro: " + replay_command(seed, schedule) +
            "\nsource:\n" + source);
    require(
        baseline.observable.starts_with("object(@0)") &&
            baseline.observable.find("builder[") != std::string::npos &&
            !baseline.output.empty(),
        "builder grammar skipped graph, Builder, or output oracle seed=" +
            std::to_string(seed));
    require(
        fuzz::same_observables(baseline, observed),
        "builder observables drifted seed=" + std::to_string(seed) +
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

Mutant mutant_source(std::size_t mutant) {
    switch (mutant) {
    case 0:
        return {"let n: i64 = 1;\n"
                "n.append(\"x\");\n0\n",
                "append requires builder receiver"};
    case 1:
        return {"let b: builder = builder();\n"
                "b.append();\n0\n",
                "append expects exactly 1 argument"};
    case 2:
        return {"let b: builder = builder();\n"
                "b.append(builder());\n0\n",
                "builder append accepts str, i64, or bool"};
    case 3:
        return {"let b: builder = builder();\n"
                "b.append(pair(1, 2));\n0\n",
                "builder append accepts str, i64, or bool"};
    case 4:
        return {"let b: builder = builder();\n"
                "b.clear(\"x\");\n0\n",
                "clear expects exactly 0 arguments"};
    case 5:
        return {"let b: builder = builder();\n"
                "b.to_str(\"x\")\n",
                "to_str expects exactly 0 arguments"};
    case 6:
        return {"builder(1)\n",
                "builder expects exactly 0 arguments"};
    case 7:
        return {"let n: i64 = 1;\n"
                "n.to_str()\n",
                "to_str requires builder receiver"};
    }
    throw std::out_of_range("builder mutant index");
}

void require_mutant_rejected(std::uint64_t seed,
                             std::size_t mutant) {
    const auto generated = mutant_source(mutant);
    const auto compiled =
        lang::frontend::compile_program(generated.source);
    require(
        !compiled.ok() && !compiled.diagnostics.empty(),
        "builder mutant unexpectedly compiled seed=" +
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
        "builder mutant omitted stable diagnostic seed=" +
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
        "builder mutant rejection lacked positioned diagnostics seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant));
}

void require_grammar(std::string_view grammar) {
    require(grammar == "builder", "expected builder grammar");
}

std::size_t parse_mutant(std::string_view text) {
    const auto parsed = fuzz::parse_seed(std::string(text));
    require(parsed < kMutantCount,
            "builder mutant index out of range");
    return static_cast<std::size_t>(parsed);
}

void pinned_artifacts(
    const std::vector<fuzz::Schedule>& schedules) {
    const auto source = generate_source(kSnapshotSeed);
    const auto module = compile_source(kSnapshotSeed, source);
    const auto outcome = fuzz::execute_once(
        module, fuzz::find_schedule(schedules, "no_stress"));
    require(outcome.ok,
            "builder representative source trapped: " +
                outcome.error);
    const auto combined =
        outcome.observable + "\nOUTPUT\n" + outcome.output;
    const auto corpus = corpus_dump();

    constexpr std::uint64_t kExpectedSourceHash =
        10'719'162'047'016'123'221ull;
    constexpr std::uint64_t kExpectedOutcomeHash =
        3'422'408'984'983'186'133ull;
    constexpr std::uint64_t kExpectedCorpusHash =
        1'386'632'754'159'073'109ull;
    const auto source_hash = fnv1a64(source);
    const auto outcome_hash = fnv1a64(combined);
    const auto corpus_hash = fnv1a64(corpus);
    require(
        source_hash == kExpectedSourceHash &&
            outcome_hash == kExpectedOutcomeHash &&
            corpus_hash == kExpectedCorpusHash,
        "builder pinned artifacts changed: source=" +
            std::to_string(source_hash) + " outcome=" +
            std::to_string(outcome_hash) + " corpus=" +
            std::to_string(corpus_hash));
}

int run(int argc, char** argv) {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "builder fuzz requires fifteen schedules");
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
        std::cerr << "[PASS] builder replay seed=" << seed
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
        std::cerr << "[PASS] builder mutant replay seed="
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
            << " <--grammar|--replay> builder --seed N "
               "--schedule NAME\n"
            << "       " << argv[0]
            << " <--grammar|--replay> builder --seed N "
               "--mutant <0..7>\n"
            << "       " << argv[0]
            << " --dump-corpus builder\n";
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
        << "[PASS] builder_pinned_seed_snapshot seed="
        << kSnapshotSeed << "\n"
        << "[PASS] lang_iteration47_string_builder_fuzz seeds="
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
        std::cerr << "[FAIL] iteration47 string builder fuzz: "
                  << error.what() << "\n";
        return 1;
    }
}
