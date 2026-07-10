#include "lang/frontend/type_checker.hpp"
#include "fuzz_common.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr std::uint64_t kSnapshotSeed = 17;
constexpr std::uint64_t kFirstCorpusSeed = 1;
constexpr std::uint64_t kCorpusSize = 32;
constexpr std::size_t kMutantCount = 4;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

struct GeneratedProgram {
    enum class TargetShape { Pair, RefArray, Map, Cycle };

    std::uint64_t seed{0};
    std::int64_t left{0};
    std::int64_t right{0};
    bool drops_target{false};
    TargetShape target_shape{TargetShape::Pair};
    std::string source;
};

GeneratedProgram generate_weak_source(std::uint64_t seed) {
    fuzz::SplitMix64 rng(seed ^ 0x26EA'6BEE'F00D'0011ull);
    GeneratedProgram generated;
    generated.seed = seed;
    generated.left = rng.small_i64();
    generated.right = rng.small_i64();
    generated.drops_target = (seed % 2) != 0;
    generated.target_shape = static_cast<GeneratedProgram::TargetShape>(
        ((seed % 4) + 3) % 4);

    std::ostringstream out;
    switch (generated.target_shape) {
    case GeneratedProgram::TargetShape::Pair:
        out << "let target: pair<i64, i64> = pair(" << generated.left << ", "
            << generated.right << ");\n"
            << "let w: weak<pair<i64, i64>> = weak(target);\n";
        if (generated.drops_target) {
            out << "target = pair(" << (generated.left + 100) << ", "
                << (generated.right + 100) << ");\n";
        }
        break;
    case GeneratedProgram::TargetShape::RefArray:
        out << "let target: [pair<i64, i64>] = [pair(" << generated.left
            << ", " << generated.right << "), pair(" << generated.right
            << ", " << generated.left << ")];\n"
            << "let w: weak<[pair<i64, i64>]> = weak(target);\n";
        if (generated.drops_target) {
            out << "target = [pair(" << (generated.left + 100) << ", "
                << (generated.right + 100) << ")];\n";
        }
        break;
    case GeneratedProgram::TargetShape::Map:
        out << "let target: map<i64, pair<i64, i64>> = map<i64, pair<i64, i64>>();\n"
            << "target[1] = pair(" << generated.left << ", "
            << generated.right << ");\n"
            << "target[2] = pair(" << generated.right << ", "
            << generated.left << ");\n"
            << "let w: weak<map<i64, pair<i64, i64>>> = weak(target);\n";
        if (generated.drops_target) {
            out << "target = map<i64, pair<i64, i64>>();\n";
        }
        break;
    case GeneratedProgram::TargetShape::Cycle:
        out << "type Node = pair<i64, Node>;\n"
            << "let target: Node = pair(" << generated.left << ", nil);\n"
            << "target.right = target;\n"
            << "let w: weak<Node> = weak(target);\n";
        if (generated.drops_target) {
            out << "target = pair(" << (generated.left + 100) << ", nil);\n";
        }
        break;
    }
    out << "let noise: i64 = 0;\n";
    for (std::size_t i = 0; i < 12; ++i) {
        out << "noise = noise + " << (1 + static_cast<std::int64_t>(rng.bounded(5)))
            << ";\n";
    }
    out << "w\n";
    generated.source = out.str();
    return generated;
}

bool schedule_clears_dropped_target(const fuzz::Schedule& schedule) {
    const std::string name = schedule.name;
    return name == "major_every_1" || name == "major_every_3" ||
           name == "major_every_7" || name == "combined";
}

std::string expected_observable(const GeneratedProgram& generated,
                                const fuzz::Schedule& schedule) {
    const bool cleared = generated.drops_target &&
                         schedule_clears_dropped_target(schedule);
    std::ostringstream out;
    out << "object(@0)\n";
    if (cleared) {
        out << "  @0 = weak(cleared)";
    } else {
        out << "  @0 = weak(alive=@1)\n";
        switch (generated.target_shape) {
        case GeneratedProgram::TargetShape::Pair:
            out << "  @1 = pair(i64(" << generated.left << "), i64("
                << generated.right << "))";
            break;
        case GeneratedProgram::TargetShape::RefArray:
            out << "  @1 = refarray[2](@2, @3)\n"
                << "  @2 = pair(i64(" << generated.left << "), i64("
                << generated.right << "))\n"
                << "  @3 = pair(i64(" << generated.right << "), i64("
                << generated.left << "))";
            break;
        case GeneratedProgram::TargetShape::Map:
            out << "  @1 = map[2]((i64(1) => @2), (i64(2) => @3))\n"
                << "  @2 = pair(i64(" << generated.left << "), i64("
                << generated.right << "))\n"
                << "  @3 = pair(i64(" << generated.right << "), i64("
                << generated.left << "))";
            break;
        case GeneratedProgram::TargetShape::Cycle:
            out << "  @1 = pair(i64(" << generated.left << "), @1)";
            break;
        }
    }
    return out.str();
}

lang::frontend::CompileResult require_compiles(
    const GeneratedProgram& generated) {
    auto compiled = lang::frontend::compile_program(generated.source);
    if (!compiled.ok()) {
        std::ostringstream out;
        out << "weak generator produced rejected source seed=" << generated.seed
            << "\nsource:\n" << generated.source << "diagnostics:\n";
        for (const auto& diagnostic : compiled.diagnostics) {
            out << diagnostic.position.line << ":" << diagnostic.position.column
                << " " << diagnostic.message << "\n";
        }
        throw std::runtime_error(out.str());
    }
    require(lang::verify(compiled.verified_module->module()),
            "frontend success did not remain verifier-accepted");
    return compiled;
}

std::string replay_command(std::uint64_t seed,
                           const fuzz::Schedule& schedule) {
    std::ostringstream out;
    out << "./build/lang_iteration26_weak_source_fuzz --grammar weak --seed "
        << seed << " --schedule " << schedule.name;
    return out.str();
}

fuzz::Outcome execute_weak_once(const lang::VerifiedModule& module,
                                const fuzz::Schedule& schedule) {
    try {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        const auto value = vm.execute(module);
        vm.heap().TEST_ONLY_validate_gc_invariants();
        if (!value.is_object() ||
            !vm.heap().TEST_ONLY_is_weak_ref(value.as_object())) {
            return fuzz::Outcome{false, {},
                                 "weak fuzzer result is not a WeakRef"};
        }

        return fuzz::Outcome{
            true,
            fuzz::canonical_object_graph(vm.heap(), value.as_object()), {},
            fuzz::output_for(vm)};
    } catch (const std::exception& error) {
        return fuzz::Outcome{false, {}, error.what()};
    }
}

void run_seed_schedule(std::uint64_t seed, const fuzz::Schedule& schedule) {
    const auto generated = generate_weak_source(seed);
    const auto compiled = require_compiles(generated);
    const auto outcome = execute_weak_once(*compiled.verified_module, schedule);
    const auto baseline = execute_weak_once(
        *compiled.verified_module,
        fuzz::find_schedule(fuzz::schedules(), "no_stress"));
    const auto expected = expected_observable(generated, schedule);
    if (!outcome.ok || !baseline.ok || outcome.observable != expected ||
        outcome.output != baseline.output) {
        std::ostringstream out;
        out << "weak source fuzz mismatch seed=" << seed
            << " schedule=" << schedule.name << "\n"
            << "repro: " << replay_command(seed, schedule) << "\n"
            << "source:\n" << generated.source
            << "expected:\n" << expected << "\n"
            << "observed:\n"
            << (outcome.ok ? outcome.observable : outcome.error) << "\n"
            << "baseline output bytes:\n"
            << fuzz::render_output_bytes(baseline.output) << "\n"
            << "observed output bytes:\n"
            << fuzz::render_output_bytes(outcome.output) << "\n";
        throw std::runtime_error(out.str());
    }
}

std::string mutant_source(std::uint64_t seed, std::size_t mutant) {
    const auto generated = generate_weak_source(seed);
    std::ostringstream out;
    switch (mutant) {
    case 0:
        out << "let w: weak<i64> = weak(1);\n"
            << "w\n";
        return out.str();
    case 1:
        out << "let w: weak<pair<i64, i64>> = weak(1);\n"
            << "w\n";
        return out.str();
    case 2:
        out << "let target: pair<i64, i64> = pair(" << generated.left << ", "
            << generated.right << ");\n"
            << "let w: weak<pair<i64, i64>> = weak(target);\n"
            << "let got: pair<i64, i64> = w.get();\n"
            << "got.left\n";
        return out.str();
    case 3:
        out << "let target: pair<i64, i64> = pair(" << generated.left << ", "
            << generated.right << ");\n"
            << "target.get()\n";
        return out.str();
    }
    throw std::runtime_error("weak mutant index out of range");
}

void require_mutant_rejected(std::uint64_t seed, std::size_t mutant) {
    const auto source = mutant_source(seed, mutant);
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok() && !compiled.diagnostics.empty(),
            "weak mutant unexpectedly compiled seed=" + std::to_string(seed) +
                " mutant=" + std::to_string(mutant));
    const bool positioned = std::any_of(
        compiled.diagnostics.begin(), compiled.diagnostics.end(),
        [&](const lang::frontend::Diagnostic& diagnostic) {
            return diagnostic.position.offset < source.size() &&
                   diagnostic.position.line > 0 && diagnostic.position.column > 0;
        });
    require(positioned,
            "weak mutant rejection lacked a positioned diagnostic");
}

void pinned_weak_source_snapshot() {
    const auto generated = generate_weak_source(kSnapshotSeed);
    const std::string expected = R"SRC(let target: pair<i64, i64> = pair(15, 36);
let w: weak<pair<i64, i64>> = weak(target);
target = pair(115, 136);
let noise: i64 = 0;
noise = noise + 5;
noise = noise + 1;
noise = noise + 3;
noise = noise + 5;
noise = noise + 5;
noise = noise + 3;
noise = noise + 1;
noise = noise + 1;
noise = noise + 2;
noise = noise + 3;
noise = noise + 5;
noise = noise + 1;
w
)SRC";
    require(generated.source == expected,
            "weak source generator snapshot changed for seed " +
                std::to_string(kSnapshotSeed) + "\nexpected:\n" + expected +
                "actual:\n" + generated.source);
}

std::size_t parse_mutant(const std::string& value) {
    const auto parsed = fuzz::parse_seed(value);
    if (parsed >= kMutantCount) {
        throw std::runtime_error("weak mutant index out of range: " + value);
    }
    return static_cast<std::size_t>(parsed);
}

void require_weak_grammar(const std::string& value) {
    if (value != "weak") {
        throw std::runtime_error("invalid source grammar: " + value);
    }
}

int run(int argc, char** argv) {
    const auto all_schedules = fuzz::schedules();
    require(all_schedules.size() == 10,
            "weak fuzz target requires exactly ten deterministic schedules");

    if (argc == 7 && std::string(argv[1]) == "--grammar" &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--schedule") {
        require_weak_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto& schedule = fuzz::find_schedule(all_schedules, argv[6]);
        run_seed_schedule(seed, schedule);
        std::cerr << "[PASS] weak source replay grammar=weak seed=" << seed
                  << " schedule=" << schedule.name << "\n";
        return 0;
    }

    if (argc == 7 && std::string(argv[1]) == "--grammar" &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--mutant") {
        require_weak_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto mutant = parse_mutant(argv[6]);
        require_mutant_rejected(seed, mutant);
        std::cerr << "[PASS] weak mutant replay grammar=weak seed=" << seed
                  << " mutant=" << mutant << "\n";
        return 0;
    }

    if (argc == 3 && std::string(argv[1]) == "--dump-corpus") {
        require_weak_grammar(argv[2]);
        for (std::uint64_t seed = kFirstCorpusSeed;
             seed < kFirstCorpusSeed + kCorpusSize; ++seed) {
            std::cout << "===== seed " << seed << " =====\n"
                      << generate_weak_source(seed).source;
        }
        return 0;
    }

    if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " --grammar weak --seed <uint64> --schedule <name>\n"
                  << "       " << argv[0]
                  << " --grammar weak --seed <uint64> --mutant <0..3>\n"
                  << "       " << argv[0] << " --dump-corpus weak\n";
        return 2;
    }

    pinned_weak_source_snapshot();
    for (std::uint64_t seed = kFirstCorpusSeed;
         seed < kFirstCorpusSeed + kCorpusSize; ++seed) {
        for (const auto& schedule : all_schedules) {
            run_seed_schedule(seed, schedule);
        }
        for (std::size_t mutant = 0; mutant < kMutantCount; ++mutant) {
            require_mutant_rejected(seed, mutant);
        }
    }
    std::cerr << "[PASS] weak_source_pinned_seed_snapshot seed="
              << kSnapshotSeed << "\n";
    std::cerr << "[PASS] lang_iteration26_weak_source_fuzz seeds="
              << kCorpusSize << " schedules=" << all_schedules.size()
              << " executions=" << (kCorpusSize * all_schedules.size())
              << " mutants=" << (kCorpusSize * kMutantCount) << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
