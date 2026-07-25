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

namespace {

constexpr std::uint64_t kSnapshotSeed = 33;
constexpr std::uint64_t kFirstCorpusSeed = 1;
constexpr std::uint64_t kCorpusSize = 32;
constexpr std::size_t kMutantCount = 7;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::int64_t generated_i64(std::uint64_t seed, std::uint64_t multiplier,
                           std::uint64_t addend) {
    return static_cast<std::int64_t>((seed * multiplier + addend) % 81) - 40;
}

std::string generate_records_source(std::uint64_t seed) {
    const auto tail_value = generated_i64(seed, 17, 3);
    const auto head_value = generated_i64(seed, 29, 5);
    const auto replacement_value = generated_i64(seed, 43, 7);
    const auto mutated_head =
        head_value + static_cast<std::int64_t>(seed % 9) - 4;
    const auto first_key = static_cast<std::int64_t>(1 + seed % 5);
    const auto second_key = first_key + 10;
    const auto head_active = seed % 2 == 1 ? "true" : "false";
    const auto replacement_active = seed % 3 == 0 ? "true" : "false";

    std::ostringstream out;
    out << "record Node {\n"
        << "  value: i64,\n"
        << "  active: bool,\n"
        << "  next: Node,\n"
        << "  payload: pair<i64, i64>\n"
        << "}\n\n"
        << "record Bundle {\n"
        << "  head: Node,\n"
        << "  items: [Node],\n"
        << "  table: map<i64, Node>,\n"
        << "  capture: fn() -> Node,\n"
        << "  observer: weak<Node>\n"
        << "}\n\n"
        << "fn sum(node: Node) -> i64 {\n"
        << "  let total: i64 = 0;\n"
        << "  if is_nil(node) {\n"
        << "    total = 0;\n"
        << "  } else {\n"
        << "    total = node.value + sum(node.next);\n"
        << "  }\n"
        << "  total\n"
        << "}\n\n"
        << "let tail: Node = Node { value: " << tail_value
        << ", active: false, next: nil, payload: pair(" << tail_value
        << ", " << head_value << ") };\n"
        << "let head: Node = Node { value: " << head_value
        << ", active: " << head_active
        << ", next: tail, payload: pair(" << head_value << ", "
        << tail_value << ") };\n"
        << "let replacement: Node = Node { value: " << replacement_value
        << ", active: " << replacement_active
        << ", next: tail, payload: pair(" << replacement_value << ", "
        << tail_value << ") };\n"
        << "head.value = " << mutated_head << ";\n"
        << "head.next = replacement;\n"
        << "let items: [Node] = [head, replacement, tail];\n"
        << "let table: map<i64, Node> = map<i64, Node>();\n"
        << "table[" << first_key << "] = head;\n"
        << "table[" << second_key << "] = tail;\n"
        << "let capture: fn() -> Node = fn() -> Node { head };\n"
        << "let observer: weak<Node> = weak(tail);\n"
        << "let bundle: Bundle = Bundle { head: head, items: items, table: table, capture: capture, observer: observer };\n"
        << "let total: i64 = sum(bundle.head);\n"
        << "for item in bundle.items {\n"
        << "  total = total + item.payload.left;\n"
        << "}\n"
        << "for key, item in bundle.table {\n"
        << "  total = total + key;\n"
        << "  if is_nil(item) {\n"
        << "    total = total + 0;\n"
        << "  } else {\n"
        << "    total = total + item.value;\n"
        << "  }\n"
        << "}\n"
        << "let captured: Node = bundle.capture();\n"
        << "if is_nil(captured) {\n"
        << "  total = -100000;\n"
        << "} else {\n"
        << "  total = total + captured.value;\n"
        << "}\n"
        << "let observed: Node = bundle.observer.get();\n"
        << "if is_nil(observed) {\n"
        << "  print(\"cleared\");\n"
        << "} else {\n"
        << "  print(to_str(observed.value));\n"
        << "}\n"
        << "print(to_str(total));\n"
        << "bundle\n";
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

lang::VerifiedModule compile_records_source(std::uint64_t seed,
                                            const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(),
            "records grammar rejected seed=" + std::to_string(seed) +
                "\nsource:\n" + source + "diagnostics:\n" +
                diagnostics_listing(compiled.diagnostics));
    require(compiled.verified_module.has_value(),
            "records grammar compilation returned no verified module");
    const auto report =
        lang::verify_with_diagnostics(compiled.verified_module->module());
    require(report.result.has_value(),
            "records grammar violated compiler/verifier agreement seed=" +
                std::to_string(seed));
    return *compiled.verified_module;
}

std::string schedule_replay(std::uint64_t seed,
                            const fuzz::Schedule& schedule) {
    std::ostringstream out;
    out << "./build/lang_iteration33_records_fuzz --grammar records --seed "
        << seed << " --schedule " << schedule.name;
    return out.str();
}

void compare_schedule(std::uint64_t seed, const std::string& source,
                      const lang::VerifiedModule& module,
                      const fuzz::Schedule& baseline_schedule,
                      const fuzz::Schedule& schedule) {
    const auto baseline = fuzz::execute_once(module, baseline_schedule);
    const auto observed = std::string(schedule.name) == baseline_schedule.name
                              ? baseline
                              : fuzz::execute_once(module, schedule);
    require(baseline.ok && observed.ok,
            "records grammar trapped seed=" + std::to_string(seed) +
                " schedule=" + schedule.name + " baseline=" +
                baseline.error + " observed=" + observed.error +
                "\nrepro: " + schedule_replay(seed, schedule) +
                "\nsource:\n" + source);
    require(!baseline.observable.empty() && !baseline.output.empty(),
            "records grammar did not exercise graph and output oracles seed=" +
                std::to_string(seed));
    require(
        fuzz::same_observables(baseline, observed),
        "records oracle drift seed=" + std::to_string(seed) +
            " schedule=" + schedule.name +
            "\nrepro: " + schedule_replay(seed, schedule) +
            "\nsource:\n" + source + "\nbaseline graph:\n" +
            baseline.observable + "\nobserved graph:\n" +
            observed.observable + "\nbaseline output bytes:\n" +
            fuzz::render_output_bytes(baseline.output) +
            "\nobserved output bytes:\n" +
            fuzz::render_output_bytes(observed.output));
}

std::string records_mutant(std::uint64_t seed, std::size_t mutant) {
    const auto value = generated_i64(seed, 17, 3);
    switch (mutant) {
    case 0:
        return "record A { value: i64 }\n"
               "record B { value: i64 }\n"
               "let a: A = A { value: 1 };\n"
               "let b: B = a;\n"
               "b\n";
    case 1:
        return "record Node { value: i64, next: Node }\n"
               "Node { next: nil, value: 1 }\n";
    case 2:
        return "record Node { value: i64, next: Node }\n"
               "Node { value: 1 }\n";
    case 3:
        return "record Node { value: i64, next: Node }\n"
               "Node { value: true, next: nil }\n";
    case 4:
        return "record Node { value: i64 }\n"
               "let node: Node = Node { value: 1 };\n"
               "node.missing\n";
    case 5:
        return "record Node { value: i64 }\n"
               "let node: Node = Node { value: 1 };\n"
               "node.value = false;\n"
               "node\n";
    case 6:
        return "record Node { value: i64, next: Node }\n"
               "fn unsafe(node: Node) -> i64 { node.value }\n"
               "let node: Node = Node { value: " + std::to_string(value) +
               ", next: nil };\n"
               "unsafe(node)\n";
    }
    throw std::runtime_error("records mutant index out of range");
}

void require_mutant_rejected(std::uint64_t seed, std::size_t mutant) {
    const auto source = records_mutant(seed, mutant);
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok() && !compiled.diagnostics.empty(),
            "records mutant unexpectedly compiled seed=" +
                std::to_string(seed) + " mutant=" +
                std::to_string(mutant) + "\nsource:\n" + source);
    const bool positioned = std::any_of(
        compiled.diagnostics.begin(), compiled.diagnostics.end(),
        [&](const lang::frontend::Diagnostic& diagnostic) {
            return diagnostic.position.offset < source.size() &&
                   diagnostic.position.line > 0 &&
                   diagnostic.position.column > 0;
        });
    require(positioned,
            "records mutant rejection lacked a positioned diagnostic seed=" +
                std::to_string(seed) + " mutant=" +
                std::to_string(mutant));
}

void pinned_records_snapshot() {
    const std::string expected = R"SRC(record Node {
  value: i64,
  active: bool,
  next: Node,
  payload: pair<i64, i64>
}

record Bundle {
  head: Node,
  items: [Node],
  table: map<i64, Node>,
  capture: fn() -> Node,
  observer: weak<Node>
}

fn sum(node: Node) -> i64 {
  let total: i64 = 0;
  if is_nil(node) {
    total = 0;
  } else {
    total = node.value + sum(node.next);
  }
  total
}

let tail: Node = Node { value: 38, active: false, next: nil, payload: pair(38, 31) };
let head: Node = Node { value: 31, active: true, next: tail, payload: pair(31, 38) };
let replacement: Node = Node { value: 9, active: true, next: tail, payload: pair(9, 38) };
head.value = 33;
head.next = replacement;
let items: [Node] = [head, replacement, tail];
let table: map<i64, Node> = map<i64, Node>();
table[4] = head;
table[14] = tail;
let capture: fn() -> Node = fn() -> Node { head };
let observer: weak<Node> = weak(tail);
let bundle: Bundle = Bundle { head: head, items: items, table: table, capture: capture, observer: observer };
let total: i64 = sum(bundle.head);
for item in bundle.items {
  total = total + item.payload.left;
}
for key, item in bundle.table {
  total = total + key;
  if is_nil(item) {
    total = total + 0;
  } else {
    total = total + item.value;
  }
}
let captured: Node = bundle.capture();
if is_nil(captured) {
  total = -100000;
} else {
  total = total + captured.value;
}
let observed: Node = bundle.observer.get();
if is_nil(observed) {
  print("cleared");
} else {
  print(to_str(observed.value));
}
print(to_str(total));
bundle
)SRC";
    require(generate_records_source(kSnapshotSeed) == expected,
            "records pinned snapshot changed for seed " +
                std::to_string(kSnapshotSeed));
}

void require_records_grammar(const std::string& grammar) {
    if (grammar != "records") {
        throw std::runtime_error("invalid source grammar: " + grammar);
    }
}

std::size_t parse_mutant(const std::string& text) {
    const auto value = fuzz::parse_seed(text);
    if (value >= kMutantCount) {
        throw std::runtime_error("records mutant index out of range: " + text);
    }
    return static_cast<std::size_t>(value);
}

int run(int argc, char** argv) {
    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "records fuzz target requires exactly fifteen deterministic schedules");
    const auto& baseline = fuzz::find_schedule(schedules, "no_stress");

    if (argc == 7 &&
        (std::string(argv[1]) == "--grammar" ||
         std::string(argv[1]) == "--replay") &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--schedule") {
        require_records_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto source = generate_records_source(seed);
        const auto module = compile_records_source(seed, source);
        const auto& schedule = fuzz::find_schedule(schedules, argv[6]);
        compare_schedule(seed, source, module, baseline, schedule);
        std::cerr << "[PASS] records replay seed=" << seed
                  << " schedule=" << schedule.name << "\n";
        return 0;
    }
    if (argc == 7 &&
        (std::string(argv[1]) == "--grammar" ||
         std::string(argv[1]) == "--replay") &&
        std::string(argv[3]) == "--seed" &&
        std::string(argv[5]) == "--mutant") {
        require_records_grammar(argv[2]);
        const auto seed = fuzz::parse_seed(argv[4]);
        const auto mutant = parse_mutant(argv[6]);
        require_mutant_rejected(seed, mutant);
        std::cerr << "[PASS] records mutant replay seed=" << seed
                  << " mutant=" << mutant << "\n";
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "--dump-corpus") {
        require_records_grammar(argv[2]);
        for (std::uint64_t seed = kFirstCorpusSeed;
             seed < kFirstCorpusSeed + kCorpusSize; ++seed) {
            std::cout << "===== seed " << seed << " =====\n"
                      << generate_records_source(seed);
        }
        return 0;
    }
    if (argc != 1) {
        std::cerr << "usage: " << argv[0]
                  << " <--grammar|--replay> records --seed N --schedule NAME\n"
                  << "       " << argv[0]
                  << " <--grammar|--replay> records --seed N --mutant <0..6>\n"
                  << "       " << argv[0]
                  << " --dump-corpus records\n";
        return 2;
    }

    pinned_records_snapshot();
    for (std::uint64_t seed = kFirstCorpusSeed;
         seed < kFirstCorpusSeed + kCorpusSize; ++seed) {
        const auto source = generate_records_source(seed);
        const auto module = compile_records_source(seed, source);
        for (const auto& schedule : schedules) {
            compare_schedule(seed, source, module, baseline, schedule);
        }
        for (std::size_t mutant = 0; mutant < kMutantCount; ++mutant) {
            require_mutant_rejected(seed, mutant);
        }
    }
    std::cerr << "[PASS] records_pinned_seed_snapshot seed="
              << kSnapshotSeed << "\n";
    std::cerr << "[PASS] lang_iteration33_records_fuzz seeds="
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
        std::cerr << "[FAIL] iteration33 records fuzz\n"
                  << error.what() << "\n";
        return 1;
    }
}
