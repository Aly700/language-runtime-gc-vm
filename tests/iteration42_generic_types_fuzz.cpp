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

constexpr std::uint64_t kFirstSeed = 42;
constexpr std::uint64_t kCorpusSize = 32;
constexpr std::uint64_t kSnapshotSeed = 42;
constexpr std::size_t kMutantCount = 12;
// `--dump-corpus generic-types` SHA-256:
// ecabdcc1db804f0a9021b8d2a040f6b5fbbf22a9f8d7fcde348a670dc9552ca1

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
    fuzz::SplitMix64 random(seed ^ 0x42A7'9D31'B5E2'6C04ull);
    const auto first = random.small_i64() + 101;
    const auto second = random.small_i64() + 211;
    const auto depth =
        static_cast<std::int64_t>(random.bounded(7) + 5);
    const bool infer_object_option = random.bounded(2) == 0;

    std::ostringstream out;
    out << "type List<T> = pair<T, List<T>>;\n"
        << "record Node<T> { value: T, next: Node<T> }\n"
        << "variant Option<T> { None(), Some(T) }\n"
        << "variant Tree<T> { Leaf(T), "
           "Branch(Tree<T>, Tree<T>) }\n"
        << "record Bundle {\n"
        << "  scalar_nodes: Node<i64>,\n"
        << "  object_nodes: Node<str>,\n"
        << "  scalar_list: List<i64>,\n"
        << "  object_list: List<str>,\n"
        << "  scalar_option: Option<i64>,\n"
        << "  object_option: Option<str>,\n"
        << "  nested: Option<List<pair<i64, str>>>,\n"
        << "  scalar_tree: Tree<i64>,\n"
        << "  object_tree: Tree<str>,\n"
        << "  node_array: [Node<str>],\n"
        << "  option_array: [Option<i64>],\n"
        << "  option_map: map<i64, Option<str>>\n"
        << "}\n\n"
        << "fn make_node<T>(value: T, next: Node<T>) -> Node<T> {\n"
        << "  Node<T> { value: value, next: next }\n"
        << "}\n"
        << "fn singleton<T>(value: T) -> List<T> {\n"
        << "  pair(value, nil)\n"
        << "}\n"
        << "fn make_some<T>(value: T) -> Option<T> {\n"
        << "  Option<T>.Some(value)\n"
        << "}\n"
        << "fn choose<T>(item: Option<T>, fallback: T) -> T {\n"
        << "  let answer: T = fallback;\n"
        << "  if is_nil(item) {\n"
        << "    answer = fallback;\n"
        << "  } else {\n"
        << "    match item {\n"
        << "      None => { answer = fallback; },\n"
        << "      Some(value) => { answer = value; }\n"
        << "    }\n"
        << "  }\n"
        << "  answer\n"
        << "}\n"
        << "fn nest<T, U>(first_value: T, second_value: U)\n"
        << "    -> Option<List<pair<T, U>>> {\n"
        << "  let item: pair<T, U> = "
           "pair(first_value, second_value);\n"
        << "  let items: List<pair<T, U>> = "
           "singleton<pair<T, U>>(item);\n"
        << "  make_some<List<pair<T, U>>>(items)\n"
        << "}\n\n";

    out << "let scalar_node: Node<i64> = "
           "make_node<i64>("
        << first << ", nil);\n"
        << "let object_node: Node<str> = "
           "make_node<str>(\"seed-\" + to_str("
        << seed << "), nil);\n"
        << "let scalar_list: List<i64> = "
           "singleton<i64>("
        << second << ");\n"
        << "let object_list: List<str> = "
           "singleton<str>(\"head\");\n"
        << "let scalar_tree: Tree<i64> = "
           "Tree<i64>.Leaf("
        << first << ");\n"
        << "let object_tree: Tree<str> = "
           "Tree<str>.Leaf(\"root\");\n"
        << "let i: i64 = 0;\n"
        << "while i < " << depth << " {\n"
        << "  scalar_node = make_node<i64>("
        << first << " + i, scalar_node);\n"
        << "  object_node = make_node<str>(to_str("
        << second << " + i), object_node);\n"
        << "  scalar_list = pair(" << first
        << " + i, scalar_list);\n"
        << "  object_list = pair(to_str(" << second
        << " + i), object_list);\n"
        << "  scalar_tree = Tree<i64>.Branch(\n"
        << "      scalar_tree, Tree<i64>.Leaf("
        << first << " + i));\n"
        << "  object_tree = Tree<str>.Branch(\n"
        << "      object_tree, Tree<str>.Leaf(to_str("
        << second << " + i)));\n"
        << "  i = i + 1;\n"
        << "}\n"
        << "let replacement: Node<str> = "
           "make_node<str>(\"replacement\", nil);\n"
        << "if is_nil(object_node) { } else {\n"
        << "  object_node.value = \"mutated-\" + to_str("
        << seed << ");\n"
        << "  object_node.next = replacement;\n"
        << "}\n"
        << "let scalar_option: Option<i64> = "
           "make_some<i64>("
        << first << " + i);\n";
    if (infer_object_option) {
        out << "let object_option: Option<str> = "
               "make_some(\"object-\" + to_str("
            << seed << "));\n";
    } else {
        out << "let object_option: Option<str> = "
               "make_some<str>(\"object-\" + to_str("
            << seed << "));\n";
    }
    out << "let nested: Option<List<pair<i64, str>>> = "
           "nest<i64, str>("
        << second << ", \"nested\");\n"
        << "let node_array: [Node<str>] = "
           "[Node<str> { value: \"array\", next: nil }];\n"
        << "let option_array: [Option<i64>] = "
           "[Option<i64>.Some("
        << first << ")];\n"
        << "let option_map: map<i64, Option<str>> = "
           "map<i64, Option<str>>();\n"
        << "option_map[" << seed
        << "] = Option<str>.Some(\"mapped\");\n"
        << "let selected_scalar: i64 = "
           "choose(scalar_option, -1);\n"
        << "let selected_object: str = "
           "choose<str>(object_option, \"fallback\");\n"
        << "print(\"seed=\" + to_str(" << seed << "));\n"
        << "print(to_str(selected_scalar));\n"
        << "print(selected_object);\n"
        << "Bundle { scalar_nodes: scalar_node, "
           "object_nodes: object_node, "
           "scalar_list: scalar_list, object_list: object_list, "
           "scalar_option: scalar_option, "
           "object_option: object_option, nested: nested, "
           "scalar_tree: scalar_tree, object_tree: object_tree, "
           "node_array: node_array, option_array: option_array, "
           "option_map: option_map }\n";
    return out.str();
}

std::string corpus_dump() {
    std::ostringstream out;
    for (std::uint64_t seed = kFirstSeed;
         seed < kFirstSeed + kCorpusSize; ++seed) {
        out << "===== seed " << seed << " =====\n"
            << generate_source(seed);
    }
    return out.str();
}

std::string replay_command(std::uint64_t seed,
                           const fuzz::Schedule& schedule) {
    return "./build/lang_iteration42_generic_types_fuzz "
           "--grammar generic-types --seed " +
           std::to_string(seed) + " --schedule " + schedule.name;
}

lang::VerifiedModule compile_source(std::uint64_t seed,
                                    const std::string& source) {
    const auto compiled = lang::frontend::compile_program(source);
    require(
        compiled.ok() && compiled.verified_module.has_value(),
        "generic-types grammar rejected seed=" +
            std::to_string(seed) + "\nrepro: " +
            replay_command(seed,
                           fuzz::find_schedule(fuzz::schedules(),
                                               "no_stress")) +
            "\nsource:\n" + source + "\ndiagnostics:\n" +
            diagnostics_listing(compiled.diagnostics));
    const auto& verified = *compiled.verified_module;
    const auto report =
        lang::verify_with_diagnostics(verified.module());
    require(
        report.result.has_value(),
        "generic-types grammar violated compiler/verifier agreement "
        "seed=" +
            std::to_string(seed) + "\nsource:\n" + source);

    const auto& module = verified.module();
    require(
        module.named_types.size() == 3,
        "generic-types grammar must emit exactly three concrete List "
        "instances seed=" +
            std::to_string(seed));
    require(
        module.record_layouts.size() == 3,
        "generic-types grammar must emit Bundle plus two concrete Node "
        "layouts seed=" +
            std::to_string(seed));
    require(
        module.variant_layouts.size() == 5,
        "generic-types grammar must emit three Option and two Tree "
        "layouts seed=" +
            std::to_string(seed));

    const auto scalar_node = std::count_if(
        module.record_layouts.begin(), module.record_layouts.end(),
        [](const lang::RecordLayout& layout) {
            return layout.reference_map ==
                   std::vector<bool>{false, true};
        });
    const auto object_node = std::count_if(
        module.record_layouts.begin(), module.record_layouts.end(),
        [](const lang::RecordLayout& layout) {
            return layout.reference_map ==
                   std::vector<bool>{true, true};
        });
    require(
        scalar_node == 1 && object_node == 1,
        "generic-types grammar lost per-instantiation Node reference "
        "precision seed=" +
            std::to_string(seed));

    const auto scalar_option = std::count_if(
        module.variant_layouts.begin(), module.variant_layouts.end(),
        [](const lang::VariantLayout& layout) {
            return layout.cases.size() == 2 &&
                   layout.cases[0].reference_map.empty() &&
                   layout.cases[1].reference_map ==
                       std::vector<bool>{false};
        });
    const auto object_options = std::count_if(
        module.variant_layouts.begin(), module.variant_layouts.end(),
        [](const lang::VariantLayout& layout) {
            return layout.cases.size() == 2 &&
                   layout.cases[0].reference_map.empty() &&
                   layout.cases[1].reference_map ==
                       std::vector<bool>{true};
        });
    require(
        scalar_option == 1 && object_options == 2,
        "generic-types grammar lost per-instantiation Option case "
        "precision seed=" +
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
        "generic-types grammar trapped seed=" +
            std::to_string(seed) + " schedule=" + schedule.name +
            " baseline=" + baseline.error +
            " observed=" + observed.error + "\nrepro: " +
            replay_command(seed, schedule) + "\nsource:\n" + source);
    require(
        !baseline.observable.empty() && !baseline.output.empty(),
        "generic-types grammar skipped graph or output oracle seed=" +
            std::to_string(seed) + "\nrepro: " +
            replay_command(seed, schedule));
    require(
        baseline.observable == observed.observable,
        "generic-types canonical graph oracle drift seed=" +
            std::to_string(seed) + " schedule=" + schedule.name +
            "\nrepro: " + replay_command(seed, schedule) +
            "\nsource:\n" + source + "\nbaseline graph:\n" +
            baseline.observable + "\nobserved graph:\n" +
            observed.observable);
    require(
        baseline.output == observed.output,
        "generic-types output oracle drift seed=" +
            std::to_string(seed) + " schedule=" + schedule.name +
            "\nrepro: " + replay_command(seed, schedule) +
            "\nbaseline output bytes:\n" +
            fuzz::render_output_bytes(baseline.output) +
            "\nobserved output bytes:\n" +
            fuzz::render_output_bytes(observed.output));
}

struct Mutant {
    std::string source;
    std::string expected;
};

Mutant mutant_source(std::uint64_t seed, std::size_t mutant) {
    const auto value =
        static_cast<std::int64_t>(seed % 31 + 1);
    switch (mutant) {
    case 0:
        return {
            "type Box<T> = pair<U, Box<T>>;\n"
            "let value: Box<i64> = nil;\n" +
                std::to_string(value) + "\n",
            "unknown type 'U'"};
    case 1:
        return {
            "record Box<T> { value: U }\n"
            "let value: Box<i64> = Box<i64> { value: 1 };\n" +
                std::to_string(value) + "\n",
            "unknown type 'U'"};
    case 2:
        return {
            "variant Box<T> { Value(U) }\n"
            "let value: Box<i64> = Box<i64>.Value(1);\n" +
                std::to_string(value) + "\n",
            "unknown type 'U'"};
    case 3:
        return {
            "type Box<T> = pair<T, Box<T>>;\n"
            "let value: Box<i64, bool> = nil;\n" +
                std::to_string(value) + "\n",
            "generic type 'Box' expects 1 type argument(s) but got 2"};
    case 4:
        return {
            "record Box<T> { value: T }\n"
            "let value: Box = nil;\n" +
                std::to_string(value) + "\n",
            "generic type 'Box' expects 1 type argument(s) but got 0"};
    case 5:
        return {
            "variant Option<T> { None(), Some(T) }\n"
            "let value: Option<i64, bool> = nil;\n" +
                std::to_string(value) + "\n",
            "generic type 'Option' expects 1 type argument(s) but got "
            "2"};
    case 6:
        return {
            "record Plain { value: i64 }\n"
            "let value: Plain<i64> = nil;\n" +
                std::to_string(value) + "\n",
            "type 'Plain' is not generic"};
    case 7:
        return {
            "type Grow<T> = pair<T, Grow<Grow<T>>>;\n"
            "let value: Grow<i64> = nil;\n" +
                std::to_string(value) + "\n",
            "generic instantiation depth limit of 32 exceeded while "
            "instantiating 'Grow'; possible polymorphic recursion"};
    case 8:
        return {
            "type Left<T> = pair<T, Right<Left<T>>>;\n"
            "type Right<T> = pair<T, Left<Right<T>>>;\n"
            "let value: Left<i64> = nil;\n" +
                std::to_string(value) + "\n",
            "generic instantiation depth limit of 32 exceeded"};
    case 9:
        return {
            "variant Option<T> { None(), Some(T) }\n"
            "fn inspect(value: Option<i64>) -> i64 {\n"
            "  let answer: i64 = 0;\n"
            "  if is_nil(value) { answer = 0; } else {\n"
            "    match value {\n"
            "      None => { answer = 1; }\n"
            "    }\n"
            "  }\n"
            "  answer\n"
            "}\n"
            "inspect(Option<i64>.Some(" +
                std::to_string(value) + "))\n",
            "non-exhaustive match; missing cases: Some"};
    case 10:
        return {
            "record Box<T> { value: T }\n"
            "let scalar: Box<i64> = Box<i64> { value: " +
                std::to_string(value) +
                " };\n"
                "let object: Box<str> = Box<str> { value: \"x\" };\n"
                "scalar = object;\n"
                "0\n",
            "cannot assign Box<str>"};
    case 11:
        return {
            "variant Option<T> { None(), Some(T) }\n"
            "let value: Option<i64> = "
            "Option<i64>.Some(\"wrong\");\n" +
                std::to_string(value) + "\n",
            "variant payload 0 expects i64 but got str"};
    }
    throw std::out_of_range("generic-types mutant index");
}

void require_mutant_rejected(std::uint64_t seed,
                             std::size_t mutant) {
    const auto generated = mutant_source(seed, mutant);
    const auto compiled =
        lang::frontend::compile_program(generated.source);
    require(
        !compiled.ok() && !compiled.diagnostics.empty(),
        "generic-types mutant unexpectedly compiled seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant) +
            "\nrepro: ./build/lang_iteration42_generic_types_fuzz "
            "--grammar generic-types --seed " +
            std::to_string(seed) + " --mutant " +
            std::to_string(mutant) + "\nsource:\n" +
            generated.source);
    const auto expected = std::find_if(
        compiled.diagnostics.begin(), compiled.diagnostics.end(),
        [&](const lang::frontend::Diagnostic& diagnostic) {
            return diagnostic.message.find(generated.expected) !=
                   std::string::npos;
        });
    require(
        expected != compiled.diagnostics.end(),
        "generic-types mutant omitted stable diagnostic '" +
            generated.expected + "' seed=" + std::to_string(seed) +
            " mutant=" + std::to_string(mutant) +
            "\ndiagnostics:\n" +
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
        "generic-types mutant rejection lacked a positioned diagnostic "
        "seed=" +
            std::to_string(seed) + " mutant=" +
            std::to_string(mutant));
}

void require_grammar(std::string_view grammar) {
    require(grammar == "generic-types",
            "expected generic-types grammar");
}

std::size_t parse_mutant(std::string_view text) {
    const auto parsed =
        fuzz::parse_seed(std::string(text));
    require(parsed < kMutantCount,
            "generic-types mutant index out of range");
    return static_cast<std::size_t>(parsed);
}

void pinned_snapshot(
    const std::vector<fuzz::Schedule>& schedules) {
    const auto source = generate_source(kSnapshotSeed);
    constexpr std::uint64_t kExpectedSourceHash =
        6'604'617'270'407'624'916ull;
    require(
        fnv1a64(source) == kExpectedSourceHash,
        "generic-types representative source pin changed: " +
            std::to_string(fnv1a64(source)));
    const auto module = compile_source(kSnapshotSeed, source);
    const auto outcome = fuzz::execute_once(
        module, fuzz::find_schedule(schedules, "no_stress"));
    require(
        outcome.ok,
        "generic-types representative source trapped: " +
            outcome.error);
    const auto combined =
        outcome.observable + "\nOUTPUT\n" + outcome.output;
    constexpr std::uint64_t kExpectedOutcomeHash =
        18'090'294'113'924'380'248ull;
    require(
        fnv1a64(combined) == kExpectedOutcomeHash,
        "generic-types representative outcome pin changed: " +
            std::to_string(fnv1a64(combined)));
}

void pinned_corpus() {
    constexpr std::uint64_t kExpectedCorpusHash =
        12'173'677'871'511'016'065ull;
    const auto corpus = corpus_dump();
    require(
        fnv1a64(corpus) == kExpectedCorpusHash,
        "generic-types full corpus pin changed: " +
            std::to_string(fnv1a64(corpus)));
}

int run(int argc, char** argv) {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "generic-types fuzz requires fifteen schedules");
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
        std::cerr << "[PASS] generic-types replay seed=" << seed
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
        std::cerr << "[PASS] generic-types mutant replay seed="
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
            << " <--grammar|--replay> generic-types --seed N "
               "--schedule NAME\n"
            << "       " << argv[0]
            << " <--grammar|--replay> generic-types --seed N "
               "--mutant <0..11>\n"
            << "       " << argv[0]
            << " --dump-corpus generic-types\n";
        return 2;
    }

    pinned_snapshot(schedules);
    pinned_corpus();
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
        << "[PASS] generic_types_pinned_seed_snapshot seed="
        << kSnapshotSeed << "\n"
        << "[PASS] lang_iteration42_generic_types_fuzz seeds="
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
        std::cerr << "[FAIL] iteration42 generic-types fuzz: "
                  << error.what() << "\n";
        return 1;
    }
}
