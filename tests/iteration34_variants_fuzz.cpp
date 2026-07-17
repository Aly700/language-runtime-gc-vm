#include "lang/frontend/type_checker.hpp"
#include "fuzz_common.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {

constexpr std::uint64_t kSnapshotSeed = 34;
constexpr std::uint64_t kFirstCorpusSeed = 1;
constexpr std::uint64_t kCorpusSize = 32;
constexpr std::size_t kMutantCount = 9;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::int64_t generated_i64(std::uint64_t seed, std::uint64_t multiplier,
                           std::uint64_t addend) {
    return static_cast<std::int64_t>((seed * multiplier + addend) % 81) + 1;
}

std::string generate_variants_source(std::uint64_t seed) {
    const auto root = generated_i64(seed, 17, 3);
    const auto first = generated_i64(seed, 29, 5);
    const auto second = generated_i64(seed, 43, 7);
    const auto branch = generated_i64(seed, 53, 11);
    const auto key = static_cast<std::int64_t>(1 + seed % 13);
    const bool deep = seed % 2 != 0;
    const bool flag = seed % 3 == 0;

    std::ostringstream out;
    out << "variant Tree { Empty(), Leaf(i64), Node(i64, Tree, Tree, pair<i64, i64>) }\n"
        << "variant Box { Nothing(), Packed(bool, Tree) }\n\n"
        << "fn sum_tree(tree: Tree) -> i64 {\n"
        << "  let answer: i64 = 0;\n"
        << "  if is_nil(tree) { answer = 0; } else {\n"
        << "  match tree {\n"
        << "    Empty => { answer = 0; },\n"
        << "    Leaf(leaf_number) => { answer = leaf_number; },\n"
        << "    Node(node_number, child_a, child_b, payload_pair) => { answer = node_number + payload_pair.left + payload_pair.right + sum_tree(child_a) + sum_tree(child_b); }\n"
        << "  }\n"
        << "  }\n"
        << "  answer\n"
        << "}\n\n"
        << "fn inspect_box(boxed: Box) -> i64 {\n"
        << "  let answer: i64 = 0;\n"
        << "  if is_nil(boxed) { answer = 0; } else {\n"
        << "  match boxed {\n"
        << "    Nothing => { answer = 0; },\n"
        << "    Packed(box_flag, box_tree) => {\n"
        << "      if is_nil(box_tree) { answer = 0; } else {\n"
        << "        match box_tree {\n"
        << "          Empty => { answer = sum_tree(box_tree); },\n"
        << "          Leaf(inner_leaf) => { answer = sum_tree(box_tree); },\n"
        << "          Node(inner_number, inner_a, inner_b, inner_pair) => { answer = sum_tree(box_tree); }\n"
        << "        }\n"
        << "      }\n"
        << "      if box_flag { answer = answer + 1; } else { answer = answer + 0; }\n"
        << "    }\n"
        << "  }\n"
        << "  }\n"
        << "  answer\n"
        << "}\n\n"
        << "let leaf_a: Tree = Tree.Leaf(" << first << ");\n"
        << "let leaf_b: Tree = Tree.Leaf(" << second << ");\n"
        << "let other: Tree = ";
    if (deep) {
        out << "Tree.Node(" << branch
            << ", leaf_b, Tree.Empty(), pair(" << second << ", " << first
            << "));\n";
    } else {
        out << "Tree.Empty();\n";
    }
    out << "let tree: Tree = Tree.Node(" << root
        << ", leaf_a, other, pair(" << first << ", " << second << "));\n"
        << "let items: [Tree] = [tree, leaf_a, Tree.Empty()];\n"
        << "let nested_items: [[Tree]] = [[tree, leaf_a], [leaf_b, Tree.Empty()]];\n"
        << "let table: map<i64, Tree> = map<i64, Tree>();\n"
        << "table[" << key << "] = leaf_b;\n"
        << "let capture: fn() -> Tree = fn() -> Tree { tree };\n"
        << "let observed_ref: weak<Tree> = weak(leaf_b);\n"
        << "let captured_tree: Tree = capture();\n"
        << "let boxed: Box = Box.Packed(" << (flag ? "true" : "false")
        << ", captured_tree);\n"
        << "let total: i64 = sum_tree(tree);\n"
        << "for item in items { total = total + sum_tree(item); }\n"
        << "for item_group in nested_items { for nested_item in item_group { total = total + sum_tree(nested_item); } }\n"
        << "for map_key, map_item in table { total = total + map_key + sum_tree(map_item); }\n"
        << "let observed_tree: Tree = observed_ref.get();\n"
        << "if is_nil(observed_tree) { print(\"cleared\"); } else { total = total + sum_tree(observed_tree); print(to_str(sum_tree(observed_tree))); }\n"
        << "total = total + inspect_box(boxed);\n"
        << "print(to_str(total));\n"
        << "tree\n";
    return out.str();
}

std::string diagnostics_listing(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::ostringstream out;
    for (const auto& diagnostic : diagnostics) {
        out << diagnostic.position.line << ":" << diagnostic.position.column
            << " " << diagnostic.message << "\n";
    }
    return out.str();
}

lang::VerifiedModule compile_variants_source(std::uint64_t seed,
                                             const std::string& source) {
    const auto repro =
        "./build/lang_iteration34_variants_fuzz --grammar variants --seed " +
        std::to_string(seed) + " --schedule no_stress";
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(),
            "variants grammar rejected seed=" + std::to_string(seed) +
                "\nrepro: " + repro + "\nsource:\n" + source +
                "diagnostics:\n" +
                diagnostics_listing(compiled.diagnostics));
    require(compiled.verified_module.has_value(),
            "variants grammar compilation returned no verified module seed=" +
                std::to_string(seed) + "\nrepro: " + repro +
                "\nsource:\n" + source);
    const auto report =
        lang::verify_with_diagnostics(compiled.verified_module->module());
    require(report.result.has_value(),
            "variants grammar violated compiler/verifier agreement seed=" +
                std::to_string(seed) + "\nrepro: " + repro +
                "\nsource:\n" + source);
    return *compiled.verified_module;
}

std::string replay_command(std::uint64_t seed,
                           const fuzz::Schedule& schedule) {
    return "./build/lang_iteration34_variants_fuzz --grammar variants --seed " +
           std::to_string(seed) + " --schedule " + schedule.name;
}

void compare_schedule(std::uint64_t seed, const std::string& source,
                      const lang::VerifiedModule& module,
                      const fuzz::Outcome& baseline,
                      const fuzz::Schedule& schedule) {
    const auto observed = std::string_view(schedule.name) == "no_stress"
                              ? baseline
                              : fuzz::execute_once(module, schedule);
    require(baseline.ok && observed.ok,
            "variants grammar trapped seed=" + std::to_string(seed) +
                " schedule=" + schedule.name + " baseline=" +
                baseline.error + " observed=" + observed.error +
                "\nrepro: " + replay_command(seed, schedule) +
                "\nsource:\n" + source);
    require(!baseline.observable.empty() && !baseline.output.empty(),
            "variants grammar did not exercise graph and output oracles seed=" +
                std::to_string(seed) + "\nrepro: " +
                replay_command(seed, schedule) + "\nsource:\n" + source);
    require(fuzz::same_observables(baseline, observed),
            "variants oracle drift seed=" + std::to_string(seed) +
                " schedule=" + schedule.name + "\nrepro: " +
                replay_command(seed, schedule) + "\nsource:\n" + source +
                "\nbaseline graph:\n" + baseline.observable +
                "\nobserved graph:\n" + observed.observable +
                "\nbaseline output bytes:\n" +
                fuzz::render_output_bytes(baseline.output) +
                "\nobserved output bytes:\n" +
                fuzz::render_output_bytes(observed.output));
}

std::string variants_mutant(std::uint64_t seed, std::size_t mutant) {
    const auto number = generated_i64(seed, 17, 3);
    const std::string prefix = "variant V { A(i64), B(bool) }\n";
    switch (mutant) {
    case 0:
        return prefix + "let v: V = V.A(" + std::to_string(number) +
               ");\nmatch v { A(x) => { print(to_str(x)); } }\n0\n";
    case 1:
        return prefix + "let v: V = V.A(1);\n"
               "match v { A(x) => {}, A(y) => {}, B(z) => {} }\n0\n";
    case 2:
        return prefix + "let v: V = V.A(1);\n"
               "match v { Missing(x) => {}, A(y) => {}, B(z) => {} }\n0\n";
    case 3:
        return prefix + "let v: V = V.A();\n0\n";
    case 4:
        return prefix + "let v: V = V.A(true);\n0\n";
    case 5:
        return "variant V { A(i64, bool) }\nlet v: V = V.A(1, true);\n"
               "match v { A(x) => {} }\n0\n";
    case 6:
        return "variant A { One(i64) }\nvariant B { One(i64) }\n"
               "let a: A = A.One(1);\nlet b: B = B.One(2);\na = b;\n0\n";
    case 7:
        return "variant V { A(i64) }\nlet v: V = nil;\n"
               "match v { A(x) => {} }\n0\n";
    case 8:
        return "variant V { A(i64) }\nlet v: V = V.A(1);\n"
               "match v { A(x) => { x = 2; } }\n0\n";
    }
    throw std::runtime_error("variants mutant index out of range");
}

void require_mutant_rejected(std::uint64_t seed, std::size_t mutant) {
    const auto source = variants_mutant(seed, mutant);
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok() && !compiled.diagnostics.empty(),
            "variants mutant unexpectedly compiled seed=" +
                std::to_string(seed) + " mutant=" + std::to_string(mutant) +
                "\nrepro: ./build/lang_iteration34_variants_fuzz --grammar variants --seed " +
                std::to_string(seed) + " --mutant " +
                std::to_string(mutant) + "\nsource:\n" + source);
    const bool positioned = std::all_of(
        compiled.diagnostics.begin(), compiled.diagnostics.end(),
        [&](const lang::frontend::Diagnostic& diagnostic) {
            return diagnostic.position.offset < source.size() &&
                   diagnostic.position.line > 0 &&
                   diagnostic.position.column > 0;
        });
    require(positioned,
            "variants mutant rejection lacked a positioned diagnostic seed=" +
                std::to_string(seed) + " mutant=" + std::to_string(mutant) +
                "\nrepro: ./build/lang_iteration34_variants_fuzz --grammar variants --seed " +
                std::to_string(seed) + " --mutant " +
                std::to_string(mutant) + "\nsource:\n" + source);
}

std::uint64_t source_hash(std::string_view source) {
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const auto byte : source) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

void pinned_variants_snapshot() {
    constexpr std::uint64_t expected = 0x28215282be144a23ull;
    const auto observed = source_hash(generate_variants_source(kSnapshotSeed));
    require(observed == expected,
            "variants pinned snapshot changed for seed " +
                std::to_string(kSnapshotSeed) + ": observed hash=" +
                std::to_string(observed) +
                "\nrepro: ./build/lang_iteration34_variants_fuzz --grammar variants --seed " +
                std::to_string(kSnapshotSeed) +
                " --schedule no_stress\nsource:\n" +
                generate_variants_source(kSnapshotSeed));
}

void require_variants_grammar(const std::string& grammar) {
    require(grammar == "variants", "invalid source grammar: " + grammar);
}

std::size_t parse_mutant(const std::string& text) {
    const auto value = fuzz::parse_seed(text);
    require(value < kMutantCount,
            "variants mutant index out of range: " + text);
    return static_cast<std::size_t>(value);
}

int run(int argc, char** argv) {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 10,
            "variants fuzz target requires exactly ten deterministic schedules");

    if (argc == 7 &&
        (std::string(argv[1]) == "--grammar" ||
         std::string(argv[1]) == "--replay") &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--schedule") {
        require_variants_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto source = generate_variants_source(seed);
        const auto module = compile_variants_source(seed, source);
        const auto baseline =
            fuzz::execute_once(module, fuzz::find_schedule(schedules, "no_stress"));
        const auto& schedule = fuzz::find_schedule(schedules, argv[6]);
        compare_schedule(seed, source, module, baseline, schedule);
        std::cerr << "[PASS] variants replay seed=" << seed
                  << " schedule=" << schedule.name << "\n";
        return 0;
    }
    if (argc == 7 &&
        (std::string(argv[1]) == "--grammar" ||
         std::string(argv[1]) == "--replay") &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--mutant") {
        require_variants_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto mutant = parse_mutant(argv[6]);
        require_mutant_rejected(seed, mutant);
        std::cerr << "[PASS] variants mutant replay seed=" << seed
                  << " mutant=" << mutant << "\n";
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--dump-corpus") {
        require_variants_grammar(argv[2]);
        for (std::uint64_t seed = kFirstCorpusSeed;
             seed < kFirstCorpusSeed + kCorpusSize; ++seed) {
            std::cout << "===== seed " << seed << " =====\n"
                      << generate_variants_source(seed);
        }
        return 0;
    }
    if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " <--grammar|--replay> variants --seed N --schedule NAME\n"
                  << "       " << argv[0]
                  << " <--grammar|--replay> variants --seed N --mutant <0..8>\n"
                  << "       " << argv[0] << " --dump-corpus variants\n";
        return 2;
    }

    pinned_variants_snapshot();
    for (std::uint64_t seed = kFirstCorpusSeed;
         seed < kFirstCorpusSeed + kCorpusSize; ++seed) {
        const auto source = generate_variants_source(seed);
        const auto module = compile_variants_source(seed, source);
        const auto baseline =
            fuzz::execute_once(module, fuzz::find_schedule(schedules, "no_stress"));
        for (const auto& schedule : schedules) {
            compare_schedule(seed, source, module, baseline, schedule);
        }
        for (std::size_t mutant = 0; mutant < kMutantCount; ++mutant) {
            require_mutant_rejected(seed, mutant);
        }
    }
    std::cerr << "[PASS] variants_pinned_seed_snapshot seed="
              << kSnapshotSeed << " hash=" << std::hex
              << source_hash(generate_variants_source(kSnapshotSeed)) << std::dec
              << "\n";
    std::cerr << "[PASS] lang_iteration34_variants_fuzz seeds="
              << kCorpusSize << " schedules=" << schedules.size()
              << " executions=" << (kCorpusSize * schedules.size())
              << " mutants=" << (kCorpusSize * kMutantCount) << "\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] iteration34 variants fuzz\n"
                  << error.what() << "\n";
        return 1;
    }
}
