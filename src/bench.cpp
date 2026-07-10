#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    std::uint64_t next() {
        std::uint64_t z = (state_ += 0x9E37'79B9'7F4A'7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D0'49BB'1331'11EBull;
        return z ^ (z >> 31);
    }

    std::uint64_t bounded(std::uint64_t exclusive_max) {
        if (exclusive_max == 0) {
            throw std::logic_error("SplitMix64 bounded called with zero");
        }
        return next() % exclusive_max;
    }

private:
    std::uint64_t state_;
};

struct CounterSnapshot {
    std::uint64_t instructions_executed{0};
    std::uint64_t allocations{0};
    std::uint64_t major_collections{0};
    std::uint64_t minor_collections{0};
    std::uint64_t objects_moved{0};
    std::uint64_t barrier_hits{0};
    std::uint64_t remembered_set_peak{0};
    std::uint64_t heap_peak_slots{0};
    std::uint64_t heap_live_objects{0};
    std::uint64_t heap_capacity_slots{0};
    std::uint64_t source_bytes{0};
    std::uint64_t module_functions{0};
    std::uint64_t bytecode_instructions{0};
    std::uint64_t stack_map_entries{0};
    std::uint64_t map_lookup_entries_examined{0};
    std::uint64_t map_descriptor_entries_scanned{0};
    std::uint64_t closure_capture_slots_scanned{0};
    std::uint64_t weak_targets_processed{0};
    std::uint64_t weak_targets_forwarded{0};
    std::uint64_t weak_targets_cleared{0};
    std::uint64_t major_weak_targets_forwarded{0};
    std::uint64_t major_weak_targets_cleared{0};
    std::uint64_t minor_weak_targets_forwarded{0};
    std::uint64_t minor_weak_targets_cleared{0};
    std::uint64_t allocation_candidate_slots_examined{0};
    std::uint64_t allocation_storage_slots_checked{0};
    std::uint64_t storage_occupancy_headers_examined{0};
    std::uint64_t heap_layout_objects_checked{0};
    std::uint64_t heap_layout_slots_checked{0};
    std::uint64_t remembered_set_entries_checked{0};
    std::uint64_t remembered_set_heap_slots_examined{0};
    std::uint64_t remembered_set_reference_fields_checked{0};
    std::uint64_t compaction_objects_copied{0};
    std::uint64_t compaction_pair_bytes{0};
    std::uint64_t compaction_scalar_array_bytes{0};
    std::uint64_t compaction_ref_array_bytes{0};
    std::uint64_t compaction_string_bytes{0};
    std::uint64_t compaction_closure_bytes{0};
    std::uint64_t compaction_map_bytes{0};
    std::uint64_t compaction_weak_ref_bytes{0};
};

bool operator==(const CounterSnapshot& lhs, const CounterSnapshot& rhs) {
    return lhs.instructions_executed == rhs.instructions_executed &&
           lhs.allocations == rhs.allocations &&
           lhs.major_collections == rhs.major_collections &&
           lhs.minor_collections == rhs.minor_collections &&
           lhs.objects_moved == rhs.objects_moved &&
           lhs.barrier_hits == rhs.barrier_hits &&
           lhs.remembered_set_peak == rhs.remembered_set_peak &&
           lhs.heap_peak_slots == rhs.heap_peak_slots &&
           lhs.heap_live_objects == rhs.heap_live_objects &&
           lhs.heap_capacity_slots == rhs.heap_capacity_slots &&
           lhs.source_bytes == rhs.source_bytes &&
           lhs.module_functions == rhs.module_functions &&
           lhs.bytecode_instructions == rhs.bytecode_instructions &&
           lhs.stack_map_entries == rhs.stack_map_entries &&
           lhs.map_lookup_entries_examined == rhs.map_lookup_entries_examined &&
           lhs.map_descriptor_entries_scanned == rhs.map_descriptor_entries_scanned &&
           lhs.closure_capture_slots_scanned == rhs.closure_capture_slots_scanned &&
           lhs.weak_targets_processed == rhs.weak_targets_processed &&
           lhs.weak_targets_forwarded == rhs.weak_targets_forwarded &&
           lhs.weak_targets_cleared == rhs.weak_targets_cleared &&
           lhs.major_weak_targets_forwarded == rhs.major_weak_targets_forwarded &&
           lhs.major_weak_targets_cleared == rhs.major_weak_targets_cleared &&
           lhs.minor_weak_targets_forwarded == rhs.minor_weak_targets_forwarded &&
           lhs.minor_weak_targets_cleared == rhs.minor_weak_targets_cleared &&
           lhs.allocation_candidate_slots_examined ==
               rhs.allocation_candidate_slots_examined &&
           lhs.allocation_storage_slots_checked ==
               rhs.allocation_storage_slots_checked &&
           lhs.storage_occupancy_headers_examined ==
               rhs.storage_occupancy_headers_examined &&
           lhs.heap_layout_objects_checked == rhs.heap_layout_objects_checked &&
           lhs.heap_layout_slots_checked == rhs.heap_layout_slots_checked &&
           lhs.remembered_set_entries_checked == rhs.remembered_set_entries_checked &&
           lhs.remembered_set_heap_slots_examined ==
               rhs.remembered_set_heap_slots_examined &&
           lhs.remembered_set_reference_fields_checked ==
               rhs.remembered_set_reference_fields_checked &&
           lhs.compaction_objects_copied == rhs.compaction_objects_copied &&
           lhs.compaction_pair_bytes == rhs.compaction_pair_bytes &&
           lhs.compaction_scalar_array_bytes == rhs.compaction_scalar_array_bytes &&
           lhs.compaction_ref_array_bytes == rhs.compaction_ref_array_bytes &&
           lhs.compaction_string_bytes == rhs.compaction_string_bytes &&
           lhs.compaction_closure_bytes == rhs.compaction_closure_bytes &&
           lhs.compaction_map_bytes == rhs.compaction_map_bytes &&
           lhs.compaction_weak_ref_bytes == rhs.compaction_weak_ref_bytes;
}

bool operator!=(const CounterSnapshot& lhs, const CounterSnapshot& rhs) {
    return !(lhs == rhs);
}

struct Workload {
    std::string name;
    std::uint64_t seed{0};
    std::string description;
    std::function<CounterSnapshot()> run;
};

struct TimedResult {
    CounterSnapshot counters;
    double median_ms{0.0};
};

struct Options {
    std::size_t repetitions{7};
    bool smoke{false};
    bool counters_only{false};
    std::optional<std::string> selected_bench;
};

void set_signature(lang::Function& function,
                   std::initializer_list<lang::ValueKind> parameters,
                   lang::ValueKind result) {
    function.signature.parameters.assign(parameters.begin(), parameters.end());
    function.signature.return_type = result;
}

std::uint64_t parse_u64(const std::string& value, const std::string& option_name) {
    std::size_t parsed = 0;
    const auto result = std::stoull(value, &parsed, 10);
    if (parsed != value.size()) {
        throw std::runtime_error("invalid integer for " + option_name + ": " + value);
    }
    return result;
}

std::string compile_error(const std::string& source,
                          const lang::frontend::CompileResult& compiled) {
    std::ostringstream out;
    out << "benchmark source failed to compile\n" << source << "\n";
    for (const auto& diagnostic : compiled.diagnostics) {
        out << diagnostic.position.line << ":" << diagnostic.position.column << " "
            << diagnostic.message << "\n";
    }
    return out.str();
}

lang::VerifiedModule compile_module_or_throw(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        throw std::runtime_error(compile_error(source, compiled));
    }
    return *compiled.verified_module;
}

std::uint64_t bytecode_instruction_count(const lang::Module& module) {
    std::uint64_t count = 0;
    for (const auto& function : module.functions) {
        count += function.code.size();
    }
    return count;
}

std::uint64_t attached_stack_map_count(const lang::Module& module) {
    std::uint64_t count = 0;
    for (const auto& function : module.functions) {
        count += function.stack_maps.size();
    }
    return count;
}

std::uint64_t verified_stack_map_count(const lang::Module& module) {
    const auto verified = lang::verify_with_stack_maps(module);
    if (!verified.has_value()) {
        throw std::runtime_error("benchmark module failed bytecode verification");
    }

    std::uint64_t count = 0;
    for (const auto& function : verified->functions) {
        count += function.stack_maps.size();
    }
    return count;
}

CounterSnapshot run_module(const lang::VerifiedModule& module, lang::gc::StressConfig stress,
                           CounterSnapshot base) {
    lang::VM vm;
    vm.set_gc_stress(stress);
    const auto result = vm.execute(module);
    (void)result;

    const auto metrics = vm.metrics();
    if (metrics.raw_module_executions != 0 || metrics.raw_function_executions != 0) {
        throw std::runtime_error("verified benchmark workload entered raw VM execution");
    }
    base.instructions_executed = metrics.instructions_executed;
    base.allocations = metrics.heap.allocations;
    base.major_collections = metrics.heap.major_collections;
    base.minor_collections = metrics.heap.minor_collections;
    base.objects_moved = metrics.heap.objects_moved;
    base.barrier_hits = metrics.heap.write_barrier_hits;
    base.remembered_set_peak = metrics.heap.remembered_set_peak;
    base.heap_peak_slots = metrics.heap.heap_peak_slots;
    base.heap_live_objects = vm.heap().live_count();
    base.heap_capacity_slots = vm.heap().capacity_slots();
    base.map_lookup_entries_examined = metrics.heap.map_lookup_entries_examined;
    base.map_descriptor_entries_scanned = metrics.heap.map_descriptor_entries_scanned;
    base.closure_capture_slots_scanned = metrics.heap.closure_capture_slots_scanned;
    base.weak_targets_processed = metrics.heap.weak_targets_processed;
    base.weak_targets_forwarded = metrics.heap.weak_targets_forwarded;
    base.weak_targets_cleared = metrics.heap.weak_targets_cleared;
    base.major_weak_targets_forwarded = metrics.heap.major_weak_targets_forwarded;
    base.major_weak_targets_cleared = metrics.heap.major_weak_targets_cleared;
    base.minor_weak_targets_forwarded = metrics.heap.minor_weak_targets_forwarded;
    base.minor_weak_targets_cleared = metrics.heap.minor_weak_targets_cleared;
    base.allocation_candidate_slots_examined =
        metrics.heap.allocation_candidate_slots_examined;
    base.allocation_storage_slots_checked =
        metrics.heap.allocation_storage_slots_checked;
    base.storage_occupancy_headers_examined =
        metrics.heap.storage_occupancy_headers_examined;
    base.heap_layout_objects_checked = metrics.heap.heap_layout_objects_checked;
    base.heap_layout_slots_checked = metrics.heap.heap_layout_slots_checked;
    base.remembered_set_entries_checked = metrics.heap.remembered_set_entries_checked;
    base.remembered_set_heap_slots_examined =
        metrics.heap.remembered_set_heap_slots_examined;
    base.remembered_set_reference_fields_checked =
        metrics.heap.remembered_set_reference_fields_checked;
    base.compaction_objects_copied = metrics.heap.compaction_objects_copied;
    base.compaction_pair_bytes = metrics.heap.compaction_pair_bytes;
    base.compaction_scalar_array_bytes = metrics.heap.compaction_scalar_array_bytes;
    base.compaction_ref_array_bytes = metrics.heap.compaction_ref_array_bytes;
    base.compaction_string_bytes = metrics.heap.compaction_string_bytes;
    base.compaction_closure_bytes = metrics.heap.compaction_closure_bytes;
    base.compaction_map_bytes = metrics.heap.compaction_map_bytes;
    base.compaction_weak_ref_bytes = metrics.heap.compaction_weak_ref_bytes;
    return base;
}

CounterSnapshot module_base_counters(const lang::Module& module, std::size_t source_bytes) {
    CounterSnapshot base;
    base.source_bytes = source_bytes;
    base.module_functions = module.functions.size();
    base.bytecode_instructions = bytecode_instruction_count(module);
    base.stack_map_entries = attached_stack_map_count(module);
    if (base.stack_map_entries == 0) {
        base.stack_map_entries = verified_stack_map_count(module);
    }
    return base;
}

std::string allocation_churn_source(std::size_t iterations, std::uint64_t seed) {
    SplitMix64 rng(seed);
    const auto bias = static_cast<std::int64_t>(rng.bounded(31) + 1);
    std::ostringstream out;
    out << "let i: i64 = 0;\n";
    out << "let tmp: pair<i64, i64> = pair(" << bias << ", " << (bias + 1) << ");\n";
    out << "while i < " << iterations << " {\n";
    out << "  tmp = pair(i + " << bias << ", i + " << (bias + 1) << ");\n";
    out << "  i = i + 1;\n";
    out << "}\n";
    out << "tmp\n";
    return out.str();
}

std::string survivor_heavy_source(std::size_t iterations, std::uint64_t seed) {
    SplitMix64 rng(seed);
    const auto bias = static_cast<std::int64_t>(rng.bounded(47) + 1);
    std::ostringstream out;
    out << "type List = pair<i64, List>;\n\n";
    out << "let i: i64 = 0;\n";
    out << "let xs: List = nil;\n";
    out << "let trash: pair<i64, i64> = pair(" << bias << ", " << (bias + 1)
        << ");\n";
    out << "while i < " << iterations << " {\n";
    out << "  trash = pair(i + " << bias << ", i);\n";
    out << "  xs = pair(i + " << bias << ", xs);\n";
    out << "  i = i + 1;\n";
    out << "}\n";
    out << "xs\n";
    return out.str();
}

std::string deep_recursion_source(std::size_t depth, std::uint64_t seed) {
    SplitMix64 rng(seed);
    const auto bias = static_cast<std::int64_t>(rng.bounded(53) + 1);
    std::ostringstream out;
    out << "fn grow(n: i64, tail: pair) -> pair {\n";
    out << "  let result: pair = tail;\n";
    out << "  if n < 1 {\n";
    out << "    result = tail;\n";
    out << "  } else {\n";
    out << "    result = grow(n + -1, pair(n + " << bias << ", tail));\n";
    out << "  }\n";
    out << "  result\n";
    out << "}\n\n";
    out << "let seed: pair = pair(" << bias << ", " << (bias + 1) << ");\n";
    out << "grow(" << depth << ", seed)\n";
    return out.str();
}

std::string verifier_compile_source(std::size_t functions, std::size_t loop_bound,
                                    std::uint64_t seed) {
    SplitMix64 rng(seed);
    std::ostringstream out;
    for (std::size_t i = 0; i < functions; ++i) {
        const auto bias = static_cast<std::int64_t>(rng.bounded(89) + 1);
        out << "fn build_" << i << "(n: i64, tail: pair) -> pair {\n";
        out << "  let i: i64 = 0;\n";
        out << "  let xs: pair = tail;\n";
        out << "  while i < n {\n";
        out << "    xs = pair(i + " << bias << ", xs);\n";
        out << "    i = i + 1;\n";
        out << "  }\n";
        out << "  xs\n";
        out << "}\n\n";
    }

    SplitMix64 seed_rng(seed ^ 0x51A7'E000ull);
    const auto seed_left = static_cast<std::int64_t>(seed_rng.bounded(101) + 1);
    out << "let xs: pair = pair(" << seed_left << ", " << (seed_left + 1) << ");\n";
    for (std::size_t i = 0; i < functions; ++i) {
        out << "xs = build_" << i << "(" << loop_bound << ", xs);\n";
    }
    out << "xs\n";
    return out.str();
}

std::string string_heavy_source(std::size_t iterations, std::uint64_t seed) {
    SplitMix64 rng(seed);
    const auto suffix = std::to_string(rng.bounded(10'000));
    std::ostringstream out;
    out << "let i: i64 = 0;\n";
    out << "let score: i64 = 0;\n";
    out << "let prefix: str = \"constant-pool-left-" << suffix << "\";\n";
    out << "let middle: str = \"-middle-\";\n";
    out << "let ending: str = \"right\";\n";
    out << "let chain: str = \"seed\";\n";
    out << "let fresh: str = \"seed\";\n";
    out << "let trash: pair<i64, i64> = pair(0, 1);\n";
    out << "while i < " << iterations << " {\n";
    out << "  chain = prefix + middle;\n";
    out << "  chain = chain + ending;\n";
    out << "  fresh = prefix + (middle + ending);\n";
    out << "  if chain == fresh {\n";
    out << "    score = score + chain[3];\n";
    out << "  } else {\n";
    out << "    score = score + -1;\n";
    out << "  }\n";
    out << "  if chain != prefix {\n";
    out << "    score = score + chain.len;\n";
    out << "  } else {\n";
    out << "    score = score + -2;\n";
    out << "  }\n";
    out << "  trash = pair(score, i);\n";
    out << "  i = i + 1;\n";
    out << "}\n";
    out << "score\n";
    return out.str();
}

std::string closure_heavy_source(std::size_t depth, std::uint64_t seed) {
    SplitMix64 rng(seed);
    const auto b0 = static_cast<std::int64_t>(rng.bounded(11) + 1);
    const auto b1 = static_cast<std::int64_t>(rng.bounded(13) + 1);
    const auto b2 = static_cast<std::int64_t>(rng.bounded(17) + 1);
    const auto b3 = static_cast<std::int64_t>(rng.bounded(19) + 1);
    std::ostringstream out;
    out << "fn apply(f: fn(i64) -> i64, value: i64) -> i64 {\n";
    out << "  f(value)\n";
    out << "}\n\n";
    out << "fn compose(outer: fn(i64) -> i64, inner: fn(i64) -> i64) "
           "-> fn(i64) -> i64 {\n";
    out << "  fn(value: i64) -> i64 {\n";
    out << "    outer(inner(value))\n";
    out << "  }\n";
    out << "}\n\n";
    out << "let b0: i64 = " << b0 << ";\n";
    out << "let b1: i64 = " << b1 << ";\n";
    out << "let b2: i64 = " << b2 << ";\n";
    out << "let b3: i64 = " << b3 << ";\n";
    out << "let i: i64 = 0;\n";
    out << "let result: i64 = 0;\n";
    out << "let base: fn(i64) -> i64 = fn(value: i64) -> i64 {\n";
    out << "  value + b0 + b1 + b2 + b3\n";
    out << "};\n";
    out << "let composed: fn(i64) -> i64 = base;\n";
    out << "while i < " << depth << " {\n";
    out << "  base = fn(value: i64) -> i64 {\n";
    out << "    value + b0 + b1 + b2 + b3 + i\n";
    out << "  };\n";
    out << "  composed = compose(base, composed);\n";
    out << "  result = apply(composed, i);\n";
    out << "  i = i + 1;\n";
    out << "}\n";
    out << "result + composed(1)\n";
    return out.str();
}

std::string map_heavy_source(std::size_t entries, std::size_t update_rounds,
                             std::uint64_t seed) {
    SplitMix64 rng(seed);
    const auto key_base = rng.bounded(100'000);
    std::ostringstream out;
    out << "let m: map<str, pair<i64, i64>> = "
           "map<str, pair<i64, i64>>();\n";
    out << "let score: i64 = 0;\n";
    for (std::size_t i = 0; i < entries; ++i) {
        const auto key_suffix = std::to_string(key_base + i);
        out << "m[\"bench-key-" << key_suffix << "\"] = pair(" << i << ", "
            << (i + 1) << ");\n";
    }
    for (std::size_t round = 0; round < update_rounds; ++round) {
        for (std::size_t i = 0; i < entries; ++i) {
            const auto key_suffix = std::to_string(key_base + i);
            out << "m[\"bench-key-\" + \"" << key_suffix << "\"] = pair("
                << (round + i + 1000) << ", " << (round + i + 2000) << ");\n";
            out << "score = score + m[\"bench-key-\" + \"" << key_suffix
                << "\"].left;\n";
        }
    }
    out << "m[\"bench-key-" << key_base << "\"] = pair(score, m.len);\n";
    out << "m\n";
    return out.str();
}

std::string mixed_graph_source(std::size_t iterations, std::uint64_t seed) {
    SplitMix64 rng(seed);
    const auto bias = static_cast<std::int64_t>(rng.bounded(29) + 1);
    std::ostringstream out;
    out << "let i: i64 = 0;\n";
    out << "let score: i64 = 0;\n";
    out << "let scalars: [i64] = array<i64>(12, " << bias << ");\n";
    out << "let node: pair<i64, i64> = pair(" << bias << ", " << bias + 1
        << ");\n";
    out << "let refs: [pair<i64, i64>] = array<pair<i64, i64>>(8, node);\n";
    out << "let label: str = \"mixed-\" + \"graph\";\n";
    out << "let table: map<str, pair<i64, i64>> = "
           "map<str, pair<i64, i64>>();\n";
    out << "table[label] = node;\n";
    out << "let weak_node: weak<pair<i64, i64>> = weak(node);\n";
    out << "let reader: fn(i64) -> i64 = fn(value: i64) -> i64 {\n";
    out << "  value + scalars[0] + refs[0].left + table[label].right\n";
    out << "};\n";
    out << "let trash: str = \"trash\";\n";
    out << "while i < " << iterations << " {\n";
    out << "  scalars[0] = i + " << bias << ";\n";
    out << "  node = pair(i + " << bias << ", score + 1);\n";
    out << "  refs[0] = node;\n";
    out << "  table[\"mixed-\" + \"graph\"] = node;\n";
    out << "  trash = label + \"-trash\";\n";
    out << "  score = reader(i) + trash[1];\n";
    out << "  i = i + 1;\n";
    out << "}\n";
    out << "let got: pair<i64, i64> = weak_node.get();\n";
    out << "if is_nil(got) {\n";
    out << "  score = score + -1000;\n";
    out << "} else {\n";
    out << "  score = score + got.left;\n";
    out << "}\n";
    out << "score\n";
    return out.str();
}

lang::Module mutation_heavy_module(std::size_t owners, std::size_t rounds,
                                   std::uint64_t seed) {
    SplitMix64 rng(seed);
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);

    auto& entry = module.functions[0];
    entry.local_count = static_cast<std::uint32_t>(owners + 1);
    set_signature(entry, {}, lang::ValueKind::Object);
    const auto temp_local = static_cast<std::int64_t>(owners);

    for (std::size_t owner = 0; owner < owners; ++owner) {
        const auto left = static_cast<std::int64_t>(rng.bounded(1000));
        const auto right = static_cast<std::int64_t>(rng.bounded(1000));
        entry.code.push_back({lang::OpCode::ConstantI64, left});
        entry.code.push_back({lang::OpCode::ConstantI64, right});
        entry.code.push_back({lang::OpCode::AllocPair, 0});
        entry.code.push_back({lang::OpCode::StoreLocal, temp_local});
        entry.code.push_back({lang::OpCode::LoadLocal, temp_local});
        entry.code.push_back({lang::OpCode::LoadLocal, temp_local});
        entry.code.push_back({lang::OpCode::AllocPair, 0});
        entry.code.push_back(
            {lang::OpCode::StoreLocal, static_cast<std::int64_t>(owner)});
    }

    entry.code.push_back({lang::OpCode::Collect, 0});

    for (std::size_t round = 0; round < rounds; ++round) {
        for (std::size_t owner = 0; owner < owners; ++owner) {
            const auto left = static_cast<std::int64_t>(rng.bounded(2000) + round);
            const auto right = static_cast<std::int64_t>(rng.bounded(2000) + owner);
            entry.code.push_back({lang::OpCode::ConstantI64, left});
            entry.code.push_back({lang::OpCode::ConstantI64, right});
            entry.code.push_back({lang::OpCode::AllocPair, 0});
            entry.code.push_back({lang::OpCode::StoreLocal, temp_local});
            entry.code.push_back(
                {lang::OpCode::LoadLocal, static_cast<std::int64_t>(owner)});
            entry.code.push_back({lang::OpCode::LoadLocal, temp_local});
            entry.code.push_back({lang::OpCode::SetLeft, 0});
        }
    }

    entry.code.push_back({lang::OpCode::LoadLocal, 0});
    entry.code.push_back({lang::OpCode::Return, 0});
    return module;
}

lang::Module weak_heavy_module(std::size_t major_rounds,
                               std::size_t minor_rounds,
                               std::uint64_t seed,
                               std::size_t minor_interval) {
    SplitMix64 rng(seed);
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);

    auto& entry = module.functions[0];
    entry.local_count = 2;
    entry.signature.return_type = lang::ValueKind::Weak;
    entry.signature.return_type_detail = lang::weak_signature(lang::pair_signature(
        lang::signature_value(lang::ValueKind::Int64),
        lang::signature_value(lang::ValueKind::Int64)));

    const auto emit_target = [&](std::int64_t bias) {
        entry.code.push_back({lang::OpCode::ConstantI64, bias});
        entry.code.push_back({lang::OpCode::ConstantI64, bias + 1});
        entry.code.push_back({lang::OpCode::AllocPair, 0});
        entry.code.push_back({lang::OpCode::StoreLocal, 0});
    };
    const auto emit_weak = [&] {
        entry.code.push_back({lang::OpCode::LoadLocal, 0});
        entry.code.push_back({lang::OpCode::AllocWeak, 0});
        entry.code.push_back({lang::OpCode::StoreLocal, 1});
    };
    const auto pad_to_minor_boundary = [&] {
        while (entry.code.size() % minor_interval != 0) {
            entry.code.push_back(
                {lang::OpCode::Jump,
                 static_cast<std::int64_t>(entry.code.size() + 1)});
        }
    };

    for (std::size_t round = 0; round < major_rounds; ++round) {
        const auto bias = static_cast<std::int64_t>(rng.bounded(100'000));
        emit_target(bias);
        emit_weak();
        entry.code.push_back({lang::OpCode::Collect, 0});
        emit_target(bias + 200'000);
        entry.code.push_back({lang::OpCode::Collect, 0});
    }

    pad_to_minor_boundary();
    for (std::size_t round = 0; round < minor_rounds; ++round) {
        const auto forwarding_bias =
            static_cast<std::int64_t>(rng.bounded(100'000) + 400'000);
        emit_target(forwarding_bias);
        emit_weak();
        pad_to_minor_boundary();

        const auto clearing_bias =
            static_cast<std::int64_t>(rng.bounded(100'000) + 600'000);
        emit_target(clearing_bias);
        emit_weak();
        emit_target(clearing_bias + 200'000);
        pad_to_minor_boundary();
    }

    entry.code.push_back({lang::OpCode::LoadLocal, 1});
    entry.code.push_back({lang::OpCode::Return, 0});
    return module;
}

Workload source_runtime_workload(std::string name, std::uint64_t seed, std::string source,
                                 lang::gc::StressConfig stress,
                                 std::string description) {
    auto module =
        std::make_shared<const lang::VerifiedModule>(compile_module_or_throw(source));
    const auto base = module_base_counters(module->module(), source.size());
    return Workload{std::move(name), seed, std::move(description),
                    [module, stress, base] { return run_module(*module, stress, base); }};
}

Workload module_runtime_workload(std::string name, std::uint64_t seed, lang::Module module,
                                 lang::gc::StressConfig stress,
                                 std::string description) {
    auto verified = lang::verify_module(std::move(module));
    if (!verified.has_value()) {
        throw std::runtime_error("benchmark module failed bytecode verification");
    }
    auto shared = std::make_shared<const lang::VerifiedModule>(std::move(*verified));
    const auto base = module_base_counters(shared->module(), 0);
    return Workload{std::move(name), seed, std::move(description),
                    [shared, stress, base] { return run_module(*shared, stress, base); }};
}

Workload weak_runtime_workload(std::uint64_t seed, lang::Module module,
                               lang::gc::StressConfig stress,
                               std::string description) {
    auto verified = lang::verify_module(std::move(module));
    if (!verified.has_value()) {
        throw std::runtime_error("weak benchmark module failed bytecode verification");
    }
    auto shared = std::make_shared<const lang::VerifiedModule>(std::move(*verified));
    const auto base = module_base_counters(shared->module(), 0);
    return Workload{
        "weak_heavy", seed, std::move(description),
        [shared, stress, base] {
            const auto counters = run_module(*shared, stress, base);
            if (counters.major_weak_targets_forwarded == 0 ||
                counters.major_weak_targets_cleared == 0 ||
                counters.minor_weak_targets_forwarded == 0 ||
                counters.minor_weak_targets_cleared == 0) {
                throw std::runtime_error(
                    "weak benchmark did not cover forwarding and clearing on both paths");
            }
            return counters;
        }};
}

Workload verifier_compile_workload(std::uint64_t seed, std::string source,
                                   std::string description) {
    return Workload{
        "verifier_compile", seed, std::move(description),
        [source = std::move(source)] {
            auto compiled = lang::frontend::compile_program(source);
            if (!compiled.ok()) {
                throw std::runtime_error(compile_error(source, compiled));
            }
            const auto& verified = *compiled.verified_module;

            CounterSnapshot counters;
            counters.source_bytes = source.size();
            counters.module_functions = verified.module().functions.size();
            counters.bytecode_instructions = bytecode_instruction_count(verified.module());
            for (const auto& function : verified.verification().functions) {
                counters.stack_map_entries += function.stack_maps.size();
            }
            return counters;
        }};
}

std::vector<Workload> build_workloads(bool smoke) {
    const auto alloc_iterations = smoke ? 120 : 1200;
    const auto survivor_iterations = smoke ? 80 : 900;
    const auto mutation_owners = smoke ? 12 : 96;
    const auto mutation_rounds = smoke ? 2 : 6;
    const auto recursion_depth = smoke ? 32 : 140;
    const auto compile_functions = smoke ? 8 : 48;
    const auto compile_loop_bound = smoke ? 3 : 7;
    const auto string_iterations = smoke ? 12 : 240;
    const auto closure_depth = smoke ? 5 : 48;
    const auto map_entries = smoke ? 8 : 96;
    const auto map_update_rounds = smoke ? 1 : 4;
    const auto weak_major_rounds = smoke ? 1 : 8;
    const auto weak_minor_rounds = smoke ? 1 : 16;
    const std::size_t weak_minor_interval = 128;
    const auto mixed_iterations = smoke ? 10 : 160;

    lang::gc::StressConfig alloc_stress;
    alloc_stress.collect_minor_every_n_instructions = 97;
    alloc_stress.collect_every_n_instructions = 503;

    lang::gc::StressConfig survivor_stress;
    survivor_stress.collect_minor_every_n_instructions = 127;
    survivor_stress.collect_every_n_instructions = 503;

    lang::gc::StressConfig recursion_stress;
    recursion_stress.collect_before_every_allocation = true;
    recursion_stress.collect_minor_every_n_instructions = 73;

    lang::gc::StressConfig string_stress;
    string_stress.collect_minor_every_n_instructions = 41;
    string_stress.collect_every_n_instructions = 173;

    lang::gc::StressConfig closure_stress;
    closure_stress.collect_minor_every_n_instructions = 53;
    closure_stress.collect_every_n_instructions = 211;

    lang::gc::StressConfig map_stress;
    map_stress.collect_minor_every_n_instructions = 113;
    map_stress.collect_every_n_instructions = 503;

    lang::gc::StressConfig mixed_stress;
    mixed_stress.collect_minor_every_n_instructions = 79;
    mixed_stress.collect_every_n_instructions = 307;

    lang::gc::StressConfig weak_stress;
    weak_stress.collect_minor_every_n_instructions = weak_minor_interval;

    std::vector<Workload> workloads;
    workloads.push_back(source_runtime_workload(
        "alloc_churn", 0xA110'C475ull,
        allocation_churn_source(alloc_iterations, 0xA110'C475ull), alloc_stress,
        "bounded loop allocating pairs; only the latest pair remains rooted"));
    workloads.push_back(source_runtime_workload(
        "survivor_heavy", 0x5A11'FEEDull,
        survivor_heavy_source(survivor_iterations, 0x5A11'FEEDull), survivor_stress,
        "retained recursive list with interleaved garbage to force compaction movement"));
    workloads.push_back(module_runtime_workload(
        "mutation_heavy", 0xBADD'CAFEull,
        mutation_heavy_module(mutation_owners, mutation_rounds, 0xBADD'CAFEull), {},
        "promoted owner pairs receive repeated young-object field writes"));
    workloads.push_back(source_runtime_workload(
        "deep_recursion_alloc", 0xDEC0'ADDEull,
        deep_recursion_source(recursion_depth, 0xDEC0'ADDEull), recursion_stress,
        "recursive list construction with allocation-triggered collections in live frames"));
    workloads.push_back(verifier_compile_workload(
        0xC0DE'600Dull,
        verifier_compile_source(compile_functions, compile_loop_bound, 0xC0DE'600Dull),
        "generated source module compiled and explicitly verified with stack maps"));
    workloads.push_back(source_runtime_workload(
        "string_heavy", 0x57A1'BEEFull,
        string_heavy_source(string_iterations, 0x57A1'BEEFull), string_stress,
        "constant-pool strings, concat chains, equality, indexing, and allocation pressure"));
    workloads.push_back(source_runtime_workload(
        "closure_heavy", 0xC105'ED00ull,
        closure_heavy_source(closure_depth, 0xC105'ED00ull), closure_stress,
        "capture-heavy closure allocation, higher-order calls, and deep composition"));
    workloads.push_back(source_runtime_workload(
        "map_heavy", 0x6A70'BEEFull,
        map_heavy_source(map_entries, map_update_rounds, 0x6A70'BEEFull), map_stress,
        "insert growth, structural string lookup, relocation, and update-in-place churn"));
    workloads.push_back(weak_runtime_workload(
        0x7EA4'BEEFull,
        weak_heavy_module(weak_major_rounds, weak_minor_rounds,
                          0x7EA4'BEEFull, weak_minor_interval),
        weak_stress,
        "exact major/minor weak-target forwarding and clearing under registry churn"));
    workloads.push_back(source_runtime_workload(
        "mixed_graph", 0xA11D'600Dull,
        mixed_graph_source(mixed_iterations, 0xA11D'600Dull), mixed_stress,
        "pairs, arrays, strings, closures, maps, and weak refs interleaved in one graph"));
    return workloads;
}

std::vector<std::pair<const char*, std::uint64_t>> counter_items(
    const CounterSnapshot& counters) {
    return {
        {"instructions_executed", counters.instructions_executed},
        {"allocations", counters.allocations},
        {"major_collections", counters.major_collections},
        {"minor_collections", counters.minor_collections},
        {"objects_moved", counters.objects_moved},
        {"barrier_hits", counters.barrier_hits},
        {"remembered_set_peak", counters.remembered_set_peak},
        {"heap_peak_slots", counters.heap_peak_slots},
        {"heap_live_objects", counters.heap_live_objects},
        {"heap_capacity_slots", counters.heap_capacity_slots},
        {"source_bytes", counters.source_bytes},
        {"module_functions", counters.module_functions},
        {"bytecode_instructions", counters.bytecode_instructions},
        {"stack_map_entries", counters.stack_map_entries},
        {"map_lookup_entries_examined", counters.map_lookup_entries_examined},
        {"map_descriptor_entries_scanned", counters.map_descriptor_entries_scanned},
        {"closure_capture_slots_scanned", counters.closure_capture_slots_scanned},
        {"weak_targets_processed", counters.weak_targets_processed},
        {"weak_targets_forwarded", counters.weak_targets_forwarded},
        {"weak_targets_cleared", counters.weak_targets_cleared},
        {"major_weak_targets_forwarded", counters.major_weak_targets_forwarded},
        {"major_weak_targets_cleared", counters.major_weak_targets_cleared},
        {"minor_weak_targets_forwarded", counters.minor_weak_targets_forwarded},
        {"minor_weak_targets_cleared", counters.minor_weak_targets_cleared},
        {"allocation_candidate_slots_examined",
         counters.allocation_candidate_slots_examined},
        {"allocation_storage_slots_checked", counters.allocation_storage_slots_checked},
        {"storage_occupancy_headers_examined",
         counters.storage_occupancy_headers_examined},
        {"heap_layout_objects_checked", counters.heap_layout_objects_checked},
        {"heap_layout_slots_checked", counters.heap_layout_slots_checked},
        {"remembered_set_entries_checked", counters.remembered_set_entries_checked},
        {"remembered_set_heap_slots_examined",
         counters.remembered_set_heap_slots_examined},
        {"remembered_set_reference_fields_checked",
         counters.remembered_set_reference_fields_checked},
        {"compaction_objects_copied", counters.compaction_objects_copied},
        {"compaction_pair_bytes", counters.compaction_pair_bytes},
        {"compaction_scalar_array_bytes", counters.compaction_scalar_array_bytes},
        {"compaction_ref_array_bytes", counters.compaction_ref_array_bytes},
        {"compaction_string_bytes", counters.compaction_string_bytes},
        {"compaction_closure_bytes", counters.compaction_closure_bytes},
        {"compaction_map_bytes", counters.compaction_map_bytes},
        {"compaction_weak_ref_bytes", counters.compaction_weak_ref_bytes},
    };
}

void print_counter_lines(const Workload& workload, const CounterSnapshot& counters) {
    for (const auto& [name, value] : counter_items(counters)) {
        std::cout << "bench=" << workload.name << " seed=" << workload.seed
                  << " kind=counter counter=" << name << " value=" << value << "\n";
    }
}

void print_time_line(const Workload& workload, std::size_t repetitions, double median_ms) {
    std::cout << "bench=" << workload.name << " seed=" << workload.seed
              << " kind=time repetitions=" << repetitions << " median_ms=" << std::fixed
              << std::setprecision(3) << median_ms << "\n";
}

TimedResult measure(const Workload& workload, std::size_t repetitions) {
    if (repetitions == 0) {
        throw std::runtime_error("--repetitions must be greater than zero");
    }

    std::vector<double> durations;
    durations.reserve(repetitions);
    std::optional<CounterSnapshot> first_counters;

    for (std::size_t i = 0; i < repetitions; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto counters = workload.run();
        const auto stop = std::chrono::steady_clock::now();
        if (!first_counters.has_value()) {
            first_counters = counters;
        } else if (counters != *first_counters) {
            throw std::runtime_error("counter drift across repetitions for " + workload.name);
        }
        durations.push_back(
            std::chrono::duration<double, std::milli>(stop - start).count());
    }

    std::sort(durations.begin(), durations.end());
    return TimedResult{*first_counters, durations[durations.size() / 2]};
}

void assert_smoke_determinism(const std::vector<Workload>& workloads) {
    for (const auto& workload : workloads) {
        const auto first = workload.run();
        const auto second = workload.run();
        if (first != second) {
            throw std::runtime_error("smoke counter determinism failed for " +
                                     workload.name);
        }
    }
    std::cout << "smoke=determinism status=ok workloads=" << workloads.size() << "\n";
}

void print_summary(const std::vector<std::pair<Workload, TimedResult>>& results,
                   std::size_t repetitions) {
    std::cout << "\nSummary: median wall time over " << repetitions
              << " repetitions; timings are informational.\n";
    for (const auto& [workload, result] : results) {
        const auto& c = result.counters;
        std::cout << "  " << workload.name << ": instructions=" << c.instructions_executed
                  << " allocations=" << c.allocations
                  << " major=" << c.major_collections
                  << " minor=" << c.minor_collections
                  << " moved=" << c.objects_moved
                  << " barrier_hits=" << c.barrier_hits
                  << " remembered_peak=" << c.remembered_set_peak
                  << " heap_peak_slots=" << c.heap_peak_slots
                  << " median_ms=" << std::fixed << std::setprecision(3)
                  << result.median_ms << "\n";
    }
}

void print_help(const char* argv0) {
    std::cout << "usage: " << argv0 << " [--repetitions N] [--bench NAME]"
              << " [--counters-only] [--smoke]\n";
}

Options parse_options(int argc, char** argv) {
    Options options;
    bool repetitions_set = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            print_help(argv[0]);
            std::exit(0);
        }
        if (arg == "--smoke") {
            options.smoke = true;
            continue;
        }
        if (arg == "--counters-only") {
            options.counters_only = true;
            continue;
        }
        if (arg == "--repetitions") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--repetitions requires a value");
            }
            options.repetitions =
                static_cast<std::size_t>(parse_u64(argv[++i], "--repetitions"));
            repetitions_set = true;
            continue;
        }
        if (arg == "--bench") {
            if (i + 1 >= argc) {
                throw std::runtime_error("--bench requires a value");
            }
            options.selected_bench = argv[++i];
            continue;
        }
        throw std::runtime_error("unknown option: " + arg);
    }

    if (options.smoke && !repetitions_set) {
        options.repetitions = 1;
    }
    return options;
}

std::vector<Workload> select_workloads(std::vector<Workload> workloads,
                                       const std::optional<std::string>& selected) {
    if (!selected.has_value()) {
        return workloads;
    }

    std::vector<Workload> filtered;
    for (auto& workload : workloads) {
        if (workload.name == *selected) {
            filtered.push_back(std::move(workload));
        }
    }
    if (filtered.empty()) {
        throw std::runtime_error("unknown benchmark: " + *selected);
    }
    return filtered;
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        auto workloads =
            select_workloads(build_workloads(options.smoke), options.selected_bench);

        if (options.smoke) {
            assert_smoke_determinism(workloads);
        }

        if (options.counters_only) {
            for (const auto& workload : workloads) {
                print_counter_lines(workload, workload.run());
            }
            return 0;
        }

        std::vector<std::pair<Workload, TimedResult>> results;
        results.reserve(workloads.size());
        for (auto& workload : workloads) {
            auto result = measure(workload, options.repetitions);
            print_counter_lines(workload, result.counters);
            print_time_line(workload, options.repetitions, result.median_ms);
            results.push_back({std::move(workload), result});
        }
        print_summary(results, options.repetitions);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "lang_bench: " << e.what() << "\n";
        return 1;
    }
}
