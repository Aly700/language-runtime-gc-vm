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
#include <vector>

namespace {

constexpr std::uint64_t kFirstSeed = 44;
constexpr std::uint64_t kCorpusSize = 32;
constexpr std::uint64_t kSnapshotSeed = 44;
constexpr std::size_t kMutantCount = 4;

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
    fuzz::SplitMix64 random(seed ^ 0x44D1'9A77'8C02'5BE3ull);
    const auto number = 10'000 + random.bounded(80'000);
    const auto token = std::to_string(number);
    const auto split = static_cast<std::size_t>(
        1 + random.bounded(token.size() - 1));
    const auto left = token.substr(0, split);
    const auto right = token.substr(split);
    const auto value =
        std::string("value-") + std::to_string(random.bounded(10'000));

    std::ostringstream out;
    out << "record InternBundle {\n"
        << "  first: str,\n"
        << "  second: str,\n"
        << "  table: map<str, str>,\n"
        << "  render: fn() -> str\n"
        << "}\n\n"
        << "fn hold(value: str) -> fn() -> str {\n"
        << "  fn() -> str { value }\n"
        << "}\n\n"
        << "let literal: str = intern(\"" << token << "\");\n"
        << "let concat_candidate: str = \"" << left << "\" + \""
        << right << "\";\n"
        << "let concatenated: str = intern(concat_candidate);\n"
        << "let raw: str = \"x" << token << "y\";\n"
        << "let sliced: str = intern(raw.sub(1, "
        << token.size() + 1 << "));\n"
        << "let table: map<str, str> = map<str, str>();\n"
        << "table[literal] = intern(\"" << value << "\");\n"
        << "let bundle: InternBundle = InternBundle {\n"
        << "  first: concatenated,\n"
        << "  second: sliced,\n"
        << "  table: table,\n"
        << "  render: hold(literal)\n"
        << "};\n"
        << "literal = \"discard-literal-" << seed << "\";\n"
        << "concatenated = \"discard-concat-" << seed << "\";\n"
        << "sliced = \"discard-slice-" << seed << "\";\n"
        << "concat_candidate = \"discard-candidate\";\n"
        << "raw = \"discard-raw\";\n"
        << "let converted: str = intern(to_str(" << number << "));\n"
        << "let late_literal: str = intern(\"" << token << "\");\n"
        << "let late_raw: str = \"z\" + to_str(" << number
        << ") + \"z\";\n"
        << "let late_slice: str = intern(late_raw.sub(1, "
        << token.size() + 1 << "));\n"
        << "let captured: str = bundle.render();\n"
        << "print(bundle.table[converted]);\n"
        << "print(captured);\n"
        << "print(to_str(converted == late_literal));\n"
        << "pair(bundle, pair(converted, pair(late_literal, "
           "pair(late_slice, captured))))\n";
    return out.str();
}

std::string corpus_dump() {
    std::ostringstream out;
    for (std::uint64_t seed = kFirstSeed;
         seed < kFirstSeed + kCorpusSize; ++seed) {
        out << "===== grammar interning seed " << seed << " =====\n"
            << generate_source(seed);
    }
    return out.str();
}

std::string replay_command(std::uint64_t seed,
                           const fuzz::Schedule& schedule) {
    return "./build/lang_iteration44_string_interning_fuzz "
           "--grammar interning --seed " +
           std::to_string(seed) + " --schedule " + schedule.name;
}

lang::VerifiedModule compile_source(std::uint64_t seed,
                                    const std::string& source) {
    const auto compiled = lang::frontend::compile_program(source);
    require(
        compiled.ok() && compiled.verified_module.has_value(),
        "interning grammar rejected seed=" + std::to_string(seed) +
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
            "interning grammar violated compiler/verifier agreement seed=" +
                std::to_string(seed));

    std::size_t intern_count = 0;
    for (const auto& function : verified.module().functions) {
        intern_count += static_cast<std::size_t>(std::count_if(
            function.code.begin(), function.code.end(),
            [](const lang::Instruction& instruction) {
                return instruction.op == lang::OpCode::StrIntern;
            }));
    }
    require(intern_count == 7,
            "interning grammar did not emit its seven explicit intern calls "
            "seed=" +
                std::to_string(seed));
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
        "interning grammar trapped seed=" + std::to_string(seed) +
            " schedule=" + schedule.name + " baseline=" +
            baseline.error + " observed=" + observed.error +
            "\nrepro: " + replay_command(seed, schedule) +
            "\nsource:\n" + source);
    require(
        !baseline.observable.empty() && !baseline.output.empty(),
        "interning grammar skipped graph or output oracle seed=" +
            std::to_string(seed));
    require(
        baseline.observable == observed.observable,
        "interning canonical graph oracle drift seed=" +
            std::to_string(seed) + " schedule=" + schedule.name +
            "\nrepro: " + replay_command(seed, schedule) +
            "\nsource:\n" + source + "\nbaseline graph:\n" +
            baseline.observable + "\nobserved graph:\n" +
            observed.observable);
    require(
        baseline.output == observed.output,
        "interning output oracle drift seed=" + std::to_string(seed) +
            " schedule=" + schedule.name +
            "\nrepro: " + replay_command(seed, schedule) +
            "\nbaseline output bytes:\n" +
            fuzz::render_output_bytes(baseline.output) +
            "\nobserved output bytes:\n" +
            fuzz::render_output_bytes(observed.output));
}

std::string mutant_source(std::uint64_t seed, std::size_t mutant) {
    switch (mutant) {
    case 0:
        return "intern(" + std::to_string(seed) + ")\n";
    case 1:
        return seed % 2 == 0 ? "intern(true)\n" : "intern(false)\n";
    case 2:
        return "intern(pair(" + std::to_string(seed) + ", " +
               std::to_string(seed + 1) + "))\n";
    case 3:
        return "intern(nil)\n";
    }
    throw std::out_of_range("interning mutant index");
}

void require_mutant_rejected(std::uint64_t seed,
                             std::size_t mutant) {
    const auto source = mutant_source(seed, mutant);
    const auto compiled = lang::frontend::compile_program(source);
    require(
        !compiled.ok() && !compiled.diagnostics.empty(),
        "interning mutant unexpectedly compiled seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant) + "\nsource:\n" + source);
    const auto stable = std::find_if(
        compiled.diagnostics.begin(), compiled.diagnostics.end(),
        [](const lang::frontend::Diagnostic& diagnostic) {
            return diagnostic.message.find("intern expects str") !=
                   std::string::npos;
        });
    require(
        stable != compiled.diagnostics.end(),
        "interning mutant omitted stable type diagnostic seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant) + "\ndiagnostics:\n" +
            diagnostics_listing(compiled.diagnostics));
    require(
        std::all_of(
            compiled.diagnostics.begin(), compiled.diagnostics.end(),
            [&](const lang::frontend::Diagnostic& diagnostic) {
                return diagnostic.position.line > 0 &&
                       diagnostic.position.column > 0 &&
                       diagnostic.position.offset < source.size();
            }),
        "interning mutant rejection lacked a positioned diagnostic seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant));
}

void require_grammar(std::string_view grammar) {
    require(grammar == "interning", "expected interning grammar");
}

std::size_t parse_mutant(std::string_view text) {
    const auto parsed = fuzz::parse_seed(std::string(text));
    require(parsed < kMutantCount,
            "interning mutant index out of range");
    return static_cast<std::size_t>(parsed);
}

void pinned_artifacts(
    const std::vector<fuzz::Schedule>& schedules) {
    const auto source = generate_source(kSnapshotSeed);
    const auto module = compile_source(kSnapshotSeed, source);
    const auto outcome = fuzz::execute_once(
        module, fuzz::find_schedule(schedules, "no_stress"));
    require(outcome.ok,
            "interning representative source trapped: " + outcome.error);
    const auto combined =
        outcome.observable + "\nOUTPUT\n" + outcome.output;
    const auto corpus = corpus_dump();

    constexpr std::uint64_t kExpectedSourceHash =
        17'523'492'481'946'191'324ull;
    constexpr std::uint64_t kExpectedOutcomeHash =
        385'766'377'391'366'824ull;
    constexpr std::uint64_t kExpectedCorpusHash =
        2'340'986'442'596'112'348ull;
    const auto source_hash = fnv1a64(source);
    const auto outcome_hash = fnv1a64(combined);
    const auto corpus_hash = fnv1a64(corpus);
    require(
        source_hash == kExpectedSourceHash &&
            outcome_hash == kExpectedOutcomeHash &&
            corpus_hash == kExpectedCorpusHash,
        "interning pinned artifacts changed: source=" +
            std::to_string(source_hash) + " outcome=" +
            std::to_string(outcome_hash) + " corpus=" +
            std::to_string(corpus_hash));
}

int run(int argc, char** argv) {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "interning fuzz requires fifteen schedules");
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
        compare_schedule(seed, source, module, baseline, schedule);
        std::cerr << "[PASS] interning replay seed=" << seed
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
        std::cerr << "[PASS] interning mutant replay seed="
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
            << " <--grammar|--replay> interning --seed N "
               "--schedule NAME\n"
            << "       " << argv[0]
            << " <--grammar|--replay> interning --seed N "
               "--mutant <0..3>\n"
            << "       " << argv[0]
            << " --dump-corpus interning\n";
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
            compare_schedule(seed, source, module, baseline, schedule);
        }
        for (std::size_t mutant = 0; mutant < kMutantCount;
             ++mutant) {
            require_mutant_rejected(seed, mutant);
        }
    }
    std::cerr
        << "[PASS] interning_pinned_seed_snapshot seed="
        << kSnapshotSeed << "\n"
        << "[PASS] lang_iteration44_string_interning_fuzz seeds="
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
        std::cerr << "[FAIL] iteration44 interning fuzz: "
                  << error.what() << "\n";
        return 1;
    }
}
