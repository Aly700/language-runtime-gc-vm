#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"
#include "fuzz_common.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

lang::ObjectId allocate_string(lang::gc::Heap& heap, const std::string& text) {
    return heap.allocate_string(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

void heap_map_preserves_insertion_order_and_updates_in_place() {
    lang::gc::Heap heap;
    auto map = heap.make_handle(heap.allocate_map(0, false, false));

    heap.map_set(map.object(), lang::Value::int64(2), lang::Value::int64(20));
    heap.map_set(map.object(), lang::Value::int64(1), lang::Value::int64(10));
    heap.map_set(map.object(), lang::Value::int64(2), lang::Value::int64(21));

    require(heap.TEST_ONLY_is_map(map.object()), "allocated object is not a map");
    require(heap.map_length(map.object()) == 2,
            "updating an existing key changed map length");
    require(heap.map_key_at(map.object(), 0).as_i64() == 2,
            "first key lost insertion order");
    require(heap.map_value_at(map.object(), 0).as_i64() == 21,
            "existing key did not update in place");
    require(heap.map_key_at(map.object(), 1).as_i64() == 1,
            "second key lost insertion order");
    require(heap.map_get(map.object(), lang::Value::int64(1)).as_i64() == 10,
            "map lookup returned wrong value");
    require(heap.map_has(map.object(), lang::Value::int64(2)),
            "MapHas missed an existing key");
    require(!heap.map_has(map.object(), lang::Value::int64(3)),
            "MapHas reported a missing key");
    heap.TEST_ONLY_validate_gc_invariants();
}

void old_map_barrier_keeps_young_string_key_and_ref_value_alive() {
    lang::gc::Heap heap;
    auto map = heap.make_handle(heap.allocate_map(7, true, true));
    heap.collect();
    require(heap.TEST_ONLY_is_old_object(map.object()),
            "map did not promote before barrier test");

    const auto key = allocate_string(heap, "young-key");
    const auto value = heap.allocate_pair(lang::Value::int64(4),
                                          lang::Value::int64(5));
    lang::gc::StressConfig stress;
    stress.collect_minor_after_every_write_barrier = true;
    heap.set_stress_config(stress);

    const auto validations_before = heap.TEST_ONLY_validation_count();
    heap.map_set(map.object(), lang::Value::object(key),
                 lang::Value::object(value));

    require(heap.metrics().write_barrier_hits == 1,
            "compound map insertion did not run exactly one barrier");
    require(heap.metrics().minor_collections == 1,
            "map barrier stress did not collect immediately");
    require(heap.TEST_ONLY_validation_count() > validations_before,
            "map barrier collection skipped invariant validation");
    require(heap.map_length(map.object()) == 1,
            "barrier collection lost map entry");
    const auto stored_key = heap.map_key_at(map.object(), 0);
    const auto stored_value = heap.map_value_at(map.object(), 0);
    require(heap.string_length(stored_key.as_object()) == 9,
            "young string key died across minor collection");
    require(heap.left(stored_value.as_object()).as_i64() == 4,
            "young reference value died across minor collection");
    heap.TEST_ONLY_validate_gc_invariants();
}

void skipped_map_barrier_is_detected_loudly() {
    lang::gc::Heap heap;
    auto map = heap.make_handle(heap.allocate_map(3, false, true));
    heap.collect();
    const auto young = heap.allocate_pair(lang::Value::int64(1),
                                          lang::Value::int64(2));
    heap.TEST_ONLY_skip_next_write_barrier_for_barrier_validator();

    bool rejected = false;
    try {
        heap.map_set(map.object(), lang::Value::int64(1),
                     lang::Value::object(young));
    } catch (const std::logic_error& error) {
        rejected = std::string(error.what()) ==
                   "old-to-young reference missing remembered-set entry";
    }
    require(rejected, "remembered-set validator accepted a missing map barrier");
}

void structural_string_lookup_survives_full_compaction() {
    lang::gc::Heap heap;
    (void)heap.allocate_pair(lang::Value::int64(-1), lang::Value::int64(-1));
    auto map = heap.make_handle(heap.allocate_map(9, true, false));
    const auto original_key = allocate_string(heap, "movement-safe");
    heap.map_set(map.object(), lang::Value::object(original_key),
                 lang::Value::int64(77));
    const auto stored_before = heap.map_key_at(map.object(), 0).as_object();

    heap.collect();
    const auto stored_after = heap.map_key_at(map.object(), 0).as_object();
    require(stored_after != stored_before,
            "test setup did not move the stored string key");

    const auto fresh_equal = allocate_string(heap, "movement-safe");
    require(heap.map_has(map.object(), lang::Value::object(fresh_equal)),
            "fresh structurally equal string missed after key movement");
    require(heap.map_get(map.object(), lang::Value::object(fresh_equal)).as_i64() ==
                77,
            "structural string lookup returned wrong value after movement");
}

void descriptor_precision_keeps_scalar_bits_opaque_and_forwards_ref_slots() {
    lang::gc::Heap heap;
    const auto doomed = heap.allocate_pair(lang::Value::int64(1),
                                           lang::Value::int64(2));
    heap.collect();
    const auto exact_bits = static_cast<std::int64_t>(doomed);

    auto scalar_map = heap.make_handle(heap.allocate_map(1, false, false));
    heap.map_set(scalar_map.object(), lang::Value::int64(8),
                 lang::Value::int64(exact_bits));
    heap.collect();
    require(heap.map_value_at(scalar_map.object(), 0).as_i64() == exact_bits,
            "scalar ObjectId-shaped bits were traced or forwarded");

    (void)heap.allocate_pair(lang::Value::int64(-2), lang::Value::int64(-3));
    auto ref_map = heap.make_handle(heap.allocate_map(2, true, true));
    const auto key = allocate_string(heap, "ref-key");
    const auto value = heap.allocate_pair(lang::Value::int64(11),
                                          lang::Value::int64(12));
    heap.map_set(ref_map.object(), lang::Value::object(key),
                 lang::Value::object(value));
    const auto old_key = heap.map_key_at(ref_map.object(), 0).as_object();
    const auto old_value = heap.map_value_at(ref_map.object(), 0).as_object();
    heap.collect();
    const auto new_key = heap.map_key_at(ref_map.object(), 0).as_object();
    const auto new_value = heap.map_value_at(ref_map.object(), 0).as_object();
    require(new_key != old_key && new_value != old_value,
            "mapped key/value reference slots were not both forwarded");
    require(heap.string_length(new_key) == 7 && heap.right(new_value).as_i64() == 12,
            "forwarded map reference slots lost payloads");
}

void growth_remains_exact_across_repeated_collections() {
    lang::gc::Heap heap;
    auto map = heap.make_handle(heap.allocate_map(4, false, false));
    for (std::int64_t i = 0; i < 12; ++i) {
        (void)heap.allocate_pair(lang::Value::int64(-i),
                                 lang::Value::int64(i));
        heap.map_set(map.object(), lang::Value::int64(i),
                     lang::Value::int64(i * 10));
        if ((i % 2) == 0) {
            heap.collect_minor();
        } else {
            heap.collect();
        }
        heap.TEST_ONLY_validate_gc_invariants();
    }
    require(heap.map_length(map.object()) == 12,
            "map growth lost entries across collections");
    for (std::size_t i = 0; i < 12; ++i) {
        require(heap.map_key_at(map.object(), i).as_i64() ==
                    static_cast<std::int64_t>(i),
                "map growth changed insertion order");
        require(heap.map_value_at(map.object(), i).as_i64() ==
                    static_cast<std::int64_t>(i * 10),
                "map growth corrupted a value");
    }
    require(heap.capacity_slots() >= 25,
            "map storage width did not grow with entry count");
}

void update_collects_old_value_and_barriers_new_young_value() {
    lang::gc::Heap heap;
    auto map = heap.make_handle(heap.allocate_map(5, false, true));
    const auto old_value = heap.allocate_pair(lang::Value::int64(1),
                                              lang::Value::int64(1));
    heap.map_set(map.object(), lang::Value::int64(1),
                 lang::Value::object(old_value));
    heap.collect();

    const auto young_value = heap.allocate_pair(lang::Value::int64(9),
                                                lang::Value::int64(10));
    const auto barriers_before = heap.metrics().write_barrier_hits;
    heap.map_set(map.object(), lang::Value::int64(1),
                 lang::Value::object(young_value));
    require(heap.metrics().write_barrier_hits == barriers_before + 1,
            "map update did not barrier the new young value");
    require(heap.map_length(map.object()) == 1,
            "map update appended instead of replacing in place");
    heap.collect();
    require(heap.live_count() == 2,
            "replaced map value remained live after major collection");
    require(heap.right(heap.map_get(map.object(), lang::Value::int64(1)).as_object())
                .as_i64() == 10,
            "map update lost the replacement value");
}

lang::MapLayout scalar_map_layout() {
    return lang::MapLayout{lang::signature_value(lang::ValueKind::Int64),
                           lang::signature_value(lang::ValueKind::Int64), false,
                           false};
}

lang::SignatureValue scalar_map_signature() {
    return lang::map_signature(lang::signature_value(lang::ValueKind::Int64),
                               lang::signature_value(lang::ValueKind::Int64));
}

lang::Module module_with_entry(lang::Function entry,
                               std::vector<lang::MapLayout> layouts = {}) {
    lang::Module module;
    module.entry_function = 0;
    module.functions.push_back(std::move(entry));
    module.map_layouts = std::move(layouts);
    return module;
}

lang::VerifierReason first_rejection_reason(const lang::Module& module) {
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value(), "expected verifier rejection");
    require(!report.diagnostics.empty(), "verifier rejection lacked diagnostics");
    return report.diagnostics.front().reason;
}

void opcodes_execute_set_get_has_and_len() {
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.signature.return_type_detail =
        lang::signature_value(lang::ValueKind::Int64);
    entry.local_count = 1;
    entry.code = {
        {lang::OpCode::AllocMap, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 4},
        {lang::OpCode::ConstantI64, 40},
        {lang::OpCode::MapSet, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 4},
        {lang::OpCode::MapHas, 0},
        {lang::OpCode::JumpIfFalse, 17},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 4},
        {lang::OpCode::MapGet, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::MapLen, 0},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::Return, 0},
    };
    auto module = module_with_entry(std::move(entry), {scalar_map_layout()});
    const auto report = lang::verify_module_with_diagnostics(module);
    require(report.module.has_value(),
            "verifier rejected valid map opcodes: " +
                (report.diagnostics.empty()
                     ? std::string("<no diagnostic>")
                     : lang::format_verifier_diagnostic(
                           report.diagnostics.front())));
    lang::VM vm;
    require(vm.execute(*report.module).as_i64() == 41,
            "map opcodes returned wrong result");
}

void verifier_reports_stable_map_reason_codes() {
    {
        lang::Function entry;
        entry.signature.return_type = lang::ValueKind::Map;
        entry.signature.return_type_detail = scalar_map_signature();
        entry.code = {{lang::OpCode::AllocMap, 1}, {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(
                    std::move(entry), {scalar_map_layout()})) ==
                    lang::VerifierReason::BadMapLayoutIndex,
                "bad map layout index used wrong reason code");
    }
    {
        lang::Function entry;
        entry.signature.return_type = lang::ValueKind::Map;
        entry.signature.return_type_detail = lang::map_signature(
            lang::pair_signature(lang::signature_value(lang::ValueKind::Int64),
                                 lang::signature_value(lang::ValueKind::Int64)),
            lang::signature_value(lang::ValueKind::Int64));
        entry.code = {{lang::OpCode::AllocMap, 0}, {lang::OpCode::Return, 0}};
        lang::MapLayout invalid{
            lang::pair_signature(lang::signature_value(lang::ValueKind::Int64),
                                 lang::signature_value(lang::ValueKind::Int64)),
            lang::signature_value(lang::ValueKind::Int64), true, false};
        require(first_rejection_reason(
                    module_with_entry(std::move(entry), {invalid})) ==
                    lang::VerifierReason::InvalidMapKeyType,
                "pair map key used wrong reason code");
    }
    {
        lang::Function entry;
        entry.signature.parameters = {lang::ValueKind::Map};
        entry.signature.parameter_types = {lang::map_signature(
            lang::pair_signature(lang::signature_value(lang::ValueKind::Int64),
                                 lang::signature_value(lang::ValueKind::Int64)),
            lang::signature_value(lang::ValueKind::Int64))};
        entry.signature.return_type = lang::ValueKind::Int64;
        entry.local_count = 1;
        entry.code = {{lang::OpCode::ConstantI64, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(std::move(entry))) ==
                    lang::VerifierReason::InvalidMapKeyType,
                "signature-only pair map key used wrong reason code");
    }
    for (const auto operation : {lang::OpCode::MapGet, lang::OpCode::MapHas}) {
        lang::Function entry;
        entry.signature.return_type = operation == lang::OpCode::MapGet
                                          ? lang::ValueKind::Int64
                                          : lang::ValueKind::Bool;
        entry.code = {{lang::OpCode::ConstantI64, 1},
                      {lang::OpCode::ConstantI64, 2},
                      {operation, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(std::move(entry))) ==
                    lang::VerifierReason::MapOperationOnNonMap,
                "MapGet/MapHas non-map receiver used wrong reason code");
    }
    {
        lang::Function entry;
        entry.signature.return_type = lang::ValueKind::Int64;
        entry.code = {{lang::OpCode::ConstantI64, 1},
                      {lang::OpCode::ConstantI64, 2},
                      {lang::OpCode::ConstantI64, 3},
                      {lang::OpCode::MapSet, 0},
                      {lang::OpCode::ConstantI64, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(std::move(entry))) ==
                    lang::VerifierReason::MapOperationOnNonMap,
                "MapSet non-map receiver used wrong reason code");
    }
    {
        lang::Function entry;
        entry.signature.return_type = lang::ValueKind::Int64;
        entry.code = {{lang::OpCode::ConstantI64, 1},
                      {lang::OpCode::MapLen, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(std::move(entry))) ==
                    lang::VerifierReason::MapOperationOnNonMap,
                "MapLen non-map receiver used wrong reason code");
    }
    {
        lang::Function entry;
        entry.signature.return_type = lang::ValueKind::Int64;
        entry.code = {{lang::OpCode::AllocMap, 0},
                      {lang::OpCode::ConstantI64, 0},
                      {lang::OpCode::ConstantI64, 1},
                      {lang::OpCode::LessI64, 0},
                      {lang::OpCode::MapGet, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(
                    std::move(entry), {scalar_map_layout()})) ==
                    lang::VerifierReason::MapKeyTypeMismatch,
                "wrong map key type used wrong reason code");
    }
    {
        lang::Function entry;
        entry.signature.return_type = lang::ValueKind::Int64;
        entry.code = {{lang::OpCode::AllocMap, 0},
                      {lang::OpCode::ConstantI64, 1},
                      {lang::OpCode::ConstantI64, 0},
                      {lang::OpCode::ConstantI64, 1},
                      {lang::OpCode::LessI64, 0},
                      {lang::OpCode::MapSet, 0},
                      {lang::OpCode::ConstantI64, 0},
                      {lang::OpCode::Return, 0}};
        require(first_rejection_reason(module_with_entry(
                    std::move(entry), {scalar_map_layout()})) ==
                    lang::VerifierReason::MapValueTypeMismatch,
                "wrong map value type used wrong reason code");
    }
}

void nested_map_signature_facts_recover_value_type() {
    const auto inner_signature = lang::map_signature(
        lang::signature_value(lang::ValueKind::Str),
        lang::signature_value(lang::ValueKind::Int64));
    lang::MapLayout outer_layout{lang::signature_value(lang::ValueKind::Int64),
                                 inner_signature, false, true};
    lang::MapLayout inner_layout{lang::signature_value(lang::ValueKind::Str),
                                 lang::signature_value(lang::ValueKind::Int64),
                                 true, false};
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.local_count = 2;
    entry.code = {{lang::OpCode::AllocMap, 0},
                  {lang::OpCode::StoreLocal, 0},
                  {lang::OpCode::AllocMap, 1},
                  {lang::OpCode::StoreLocal, 1},
                  {lang::OpCode::LoadLocal, 0},
                  {lang::OpCode::ConstantI64, 7},
                  {lang::OpCode::LoadLocal, 1},
                  {lang::OpCode::MapSet, 0},
                  {lang::OpCode::LoadLocal, 0},
                  {lang::OpCode::ConstantI64, 7},
                  {lang::OpCode::MapGet, 0},
                  {lang::OpCode::MapLen, 0},
                  {lang::OpCode::Return, 0}};
    const auto verified = lang::verify_module(module_with_entry(
        std::move(entry), {outer_layout, inner_layout}));
    require(verified.has_value(),
            "nested MapGet failed to recover the map value type");
    lang::VM vm;
    require(vm.execute(*verified).as_i64() == 0,
            "nested map value executed incorrectly");
}

void missing_map_key_traps_with_stable_diagnostic() {
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.code = {{lang::OpCode::AllocMap, 0},
                  {lang::OpCode::ConstantI64, 99},
                  {lang::OpCode::MapGet, 0},
                  {lang::OpCode::Return, 0}};
    const auto verified = lang::verify_module(
        module_with_entry(std::move(entry), {scalar_map_layout()}));
    require(verified.has_value(), "missing-key program failed verification");
    lang::VM vm;
    bool trapped = false;
    try {
        (void)vm.execute(*verified);
    } catch (const std::out_of_range& error) {
        trapped = std::string(error.what()) == "map key not found";
    }
    require(trapped, "MapGet missing-key trap diagnostic drifted");
}

void vm_growth_survives_gc_every_instruction() {
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Map;
    entry.signature.return_type_detail = scalar_map_signature();
    entry.local_count = 1;
    entry.code.push_back({lang::OpCode::AllocMap, 0});
    entry.code.push_back({lang::OpCode::StoreLocal, 0});
    for (std::int64_t i = 0; i < 10; ++i) {
        entry.code.push_back({lang::OpCode::LoadLocal, 0});
        entry.code.push_back({lang::OpCode::ConstantI64, i});
        entry.code.push_back({lang::OpCode::ConstantI64, i * 3});
        entry.code.push_back({lang::OpCode::MapSet, 0});
    }
    entry.code.push_back({lang::OpCode::LoadLocal, 0});
    entry.code.push_back({lang::OpCode::Return, 0});
    const auto verified = lang::verify_module(
        module_with_entry(std::move(entry), {scalar_map_layout()}));
    require(verified.has_value(), "map growth program failed verification");

    lang::VM vm;
    lang::gc::StressConfig stress;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    vm.set_gc_stress(stress);
    const auto result = vm.execute(*verified);
    require(vm.heap().map_length(result.as_object()) == 10,
            "GC-every-instruction growth lost entries");
    for (std::size_t i = 0; i < 10; ++i) {
        require(vm.heap().map_key_at(result.as_object(), i).as_i64() ==
                    static_cast<std::int64_t>(i),
                "GC-every-instruction growth changed insertion order");
    }
}

std::string frontend_diagnostics(
    const std::vector<lang::frontend::Diagnostic>& diagnostics) {
    std::string rendered;
    for (const auto& diagnostic : diagnostics) {
        rendered += std::to_string(diagnostic.position.line) + ":" +
                    std::to_string(diagnostic.position.column) + " " +
                    diagnostic.message + "\n";
    }
    return rendered;
}

lang::Value execute_source(const std::string& source,
                           lang::gc::StressConfig stress, lang::VM& vm) {
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(), "source failed to compile:\n" + source +
                               "\n" +
                               frontend_diagnostics(compiled.diagnostics));
    vm.set_gc_stress(stress);
    return vm.execute(*compiled.verified_module);
}

void require_source_error(const std::string& source, std::size_t line,
                          std::size_t column,
                          const std::string& message_fragment) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "invalid map source unexpectedly compiled:\n" + source);
    bool found = false;
    for (const auto& diagnostic : compiled.diagnostics) {
        found = found ||
                (diagnostic.position.line == line &&
                 diagnostic.position.column == column &&
                 diagnostic.message.find(message_fragment) != std::string::npos);
    }
    require(found, "missing positioned map diagnostic '" + message_fragment +
                       "' at " + std::to_string(line) + ":" +
                       std::to_string(column) + "\nactual:\n" +
                       frontend_diagnostics(compiled.diagnostics));
}

void frontend_surface_lowers_to_precise_layouts() {
    const std::string source = R"SRC(let counts: map<str, i64> = map<str, i64>();
counts["first"] = 10;
counts["second"] = 20;
counts["first"] = 11;
let present: bool = counts.has("first");
let total: i64 = counts.len;
if present {
  total = total + counts["first"];
} else {
  total = 0;
}
total
)SRC";
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(), "map surface source failed to compile:\n" +
                               frontend_diagnostics(compiled.diagnostics));
    const auto& module = compiled.verified_module->module();
    require(module.map_layouts.size() == 1,
            "empty map constructor did not emit one layout");
    require(module.map_layouts[0].key_is_ref &&
                !module.map_layouts[0].value_is_ref,
            "map layout reference flags disagree with map<str, i64>");
    lang::VM vm;
    require(vm.execute(*compiled.verified_module).as_i64() == 13,
            "frontend map surface returned wrong result");

    auto map_result = lang::frontend::compile_program(
        "map<bool, map<str, i64>>()\n");
    require(map_result.ok() &&
                map_result.result_type == lang::frontend::Type::Map,
            "frontend did not expose Map as the result type");
}

void frontend_reports_positioned_map_misuse() {
    require_source_error(
        "let m: map<pair<i64, i64>, i64> = map<pair<i64, i64>, i64>();\n0\n",
        1, 12, "map key type must be i64, bool, or str");
    require_source_error("let x: i64 = 1;\nx[0]\n", 2, 2,
                         "indexing requires array or str or map");
    require_source_error("let x: i64 = 1;\nx.has(0)\n", 2, 3,
                         "has requires map");
    require_source_error(
        "let m: map<i64, i64> = map<i64, i64>();\nm[\"bad\"]\n", 2,
        3, "map key expects i64 but got str");
    require_source_error(
        "let m: map<i64, i64> = map<i64, i64>();\nm[\"bad\"] = 1;\n0\n",
        2, 3, "map key expects i64 but got str");
    require_source_error(
        "let m: map<i64, i64> = map<i64, i64>();\nm[1] = true;\n0\n", 2,
        6, "cannot assign bool to map value of type i64");
}

void frontend_structural_keys_and_nil_values_survive_stress() {
    const std::string source = R"SRC(type List = pair<i64, List>;
let values: map<str, List> = map<str, List>();
values["same-key"] = nil;
let fresh: str = "same" + "-key";
let found: bool = values.has(fresh);
let item: List = values[fresh];
let score: i64 = 0;
if is_nil(item) {
  score = 41;
} else {
  score = item.left;
}
if found {
  score = score + values.len;
} else {
  score = 0;
}
score
)SRC";
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    stress.collect_minor_after_every_write_barrier = true;
    lang::VM vm;
    require(execute_source(source, stress, vm).as_i64() == 42,
            "frontend structural/nil map program failed under stress");
}

void maps_compose_with_arrays_pairs_closures_and_functions() {
    const std::string source = R"SRC(fn increment(value: i64) -> i64 {
  value + 1
}

let functions: map<str, fn(i64) -> i64> = map<str, fn(i64) -> i64>();
functions["inc"] = increment;
let maps: [map<str, fn(i64) -> i64>] = [functions];
let holder: pair<map<str, fn(i64) -> i64>, map<str, fn(i64) -> i64>> = pair(maps[0], functions);
let captured: fn(i64) -> i64 = fn(value: i64) -> i64 {
  holder.left["inc"](value)
};
let i: i64 = 0;
let churn: pair<i64, i64> = pair(0, 0);
while i < 8 {
  churn = pair(i, i + 1);
  i = i + 1;
}
captured(41)
)SRC";
    lang::gc::StressConfig stress;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    stress.collect_minor_after_every_write_barrier = true;
    lang::VM vm;
    require(execute_source(source, stress, vm).as_i64() == 42,
            "map composition failed after compaction");
}

std::string generate_maps_source(std::uint64_t seed) {
    const auto bias = static_cast<std::int64_t>((seed * 17) % 41) - 20;
    std::vector<std::string> keys;
    for (std::size_t i = 0; i < 4; ++i) {
        keys.push_back("k" + std::to_string(i) + "-" +
                       std::to_string((seed * 31 + i * 7) % 97));
    }

    std::ostringstream out;
    out << "let m: map<str, pair<i64, i64>> = map<str, pair<i64, i64>>();\n";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const auto signed_index = static_cast<std::int64_t>(i);
        out << "m[\"" << keys[i] << "\"] = pair("
            << (bias + signed_index) << ", "
            << (bias + signed_index + 100) << ");\n";
    }
    const auto update = static_cast<std::size_t>(seed % keys.size());
    out << "m[\"" << keys[update] << "\"] = pair(" << (bias + 1000)
        << ", " << (bias + 1001) << ");\n";
    out << "m\n";
    return out.str();
}

lang::VerifiedModule compile_fuzz_map_source(std::uint64_t seed) {
    const auto source = generate_maps_source(seed);
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(),
            "maps source generator emitted rejected source for seed " +
                std::to_string(seed) + "\n" + source +
                frontend_diagnostics(compiled.diagnostics));
    const auto reverified =
        lang::verify_with_diagnostics(compiled.verified_module->module());
    require(reverified.result.has_value(),
            "maps source generator emitted verifier-rejected bytecode for seed " +
                std::to_string(seed));
    return *compiled.verified_module;
}

void run_maps_seed_schedule(std::uint64_t seed,
                            const fuzz::Schedule& schedule) {
    const auto verified = compile_fuzz_map_source(seed);
    const auto all_schedules = fuzz::schedules();
    const auto baseline = fuzz::execute_once(
        verified, fuzz::find_schedule(all_schedules, "no_stress"));
    const auto observed = std::string(schedule.name) == "no_stress"
                              ? baseline
                              : fuzz::execute_once(verified, schedule);
    require(baseline.ok && observed.ok,
            "maps source fuzz trapped for seed " + std::to_string(seed) +
                " schedule=" + schedule.name + " baseline=" + baseline.error +
                " observed=" + observed.error);
    require(fuzz::same_observables(baseline, observed),
            "maps source fuzz observable drift for seed " +
                std::to_string(seed) + " schedule=" + schedule.name +
                "\nbaseline:\n" + baseline.observable + "\nobserved:\n" +
                observed.observable + "\nbaseline output bytes:\n" +
                fuzz::render_output_bytes(baseline.output) +
                "\nobserved output bytes:\n" +
                fuzz::render_output_bytes(observed.output));
}

std::vector<std::string> map_mutants(std::uint64_t seed) {
    const auto value = static_cast<std::int64_t>(seed % 17);
    return {
        "let m: map<pair<i64, i64>, i64> = map<pair<i64, i64>, i64>();\n0\n",
        "let m: map<i64, bool> = map<i64, i64>();\n0\n",
        "let x: i64 = " + std::to_string(value) + ";\nx[0]\n",
        "let m: map<i64, i64> = map<i64, i64>();\nm[\"bad\"] = 1;\n0\n",
        "let m: map<i64, i64> = map<i64, i64>();\nm[1] = true;\n0\n",
        "let m: map<str, i64> = map<str, i64>();\nlet x: bool = m[\"key\"];\n0\n",
    };
}

void require_map_mutant_rejected(std::uint64_t seed, std::size_t mutant) {
    const auto mutants = map_mutants(seed);
    require(mutant < mutants.size(), "map mutant index out of range");
    const auto compiled = lang::frontend::compile_program(mutants[mutant]);
    require(!compiled.ok(), "map mutant unexpectedly compiled seed=" +
                                std::to_string(seed) + " mutant=" +
                                std::to_string(mutant) + "\n" +
                                mutants[mutant]);
}

void canonical_oracle_renders_maps_as_ordered_key_value_lists() {
    const std::string source = R"SRC(let m: map<str, pair<i64, i64>> = map<str, pair<i64, i64>>();
m["a"] = pair(1, 2);
m["b"] = pair(3, 4);
m["a"] = pair(9, 10);
m
)SRC";
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(), "canonical map source failed to compile");
    lang::VM vm;
    const auto result = vm.execute(*compiled.verified_module);
    const std::string expected =
        "object(@0)\n"
        "  @0 = map[2]((@1 => @2), (@3 => @4))\n"
        "  @1 = str[1](61)\n"
        "  @2 = pair(i64(9), i64(10))\n"
        "  @3 = str[1](62)\n"
        "  @4 = pair(i64(3), i64(4))";
    const auto actual = fuzz::observable_for(vm, result);
    require(actual == expected,
            "canonical map oracle is not an ordered key/value list\nactual:\n" +
                actual);
}

void maps_source_fuzz_pinned_snapshot() {
    const std::string expected = R"SRC(let m: map<str, pair<i64, i64>> = map<str, pair<i64, i64>>();
m["k0-42"] = pair(-18, 82);
m["k1-49"] = pair(-17, 83);
m["k2-56"] = pair(-16, 84);
m["k3-63"] = pair(-15, 85);
m["k1-49"] = pair(982, 983);
m
)SRC";
    require(generate_maps_source(17) == expected,
            "maps source pinned snapshot changed\nactual:\n" +
                generate_maps_source(17));
}

void maps_source_fuzz_corpus_and_mutants() {
    const auto all_schedules = fuzz::schedules();
    require(all_schedules.size() == 10,
            "maps source fuzz requires exactly ten deterministic schedules");
    for (std::uint64_t seed = 1; seed <= 10; ++seed) {
        for (const auto& schedule : all_schedules) {
            run_maps_seed_schedule(seed, schedule);
        }
        for (std::size_t mutant = 0; mutant < map_mutants(seed).size();
             ++mutant) {
            require_map_mutant_rejected(seed, mutant);
        }
    }
}

using Test = std::pair<const char*, void (*)()>;

} // namespace

int main(int argc, char** argv) {
    try {
        const auto all_schedules = fuzz::schedules();
        if (argc == 7 && std::string(argv[1]) == "--grammar" &&
            std::string(argv[2]) == "maps" &&
            std::string(argv[3]) == "--seed" &&
            std::string(argv[5]) == "--schedule") {
            const auto seed = fuzz::parse_seed(argv[4]);
            const auto& schedule = fuzz::find_schedule(all_schedules, argv[6]);
            run_maps_seed_schedule(seed, schedule);
            std::cerr << "[PASS] maps source replay seed=" << seed
                      << " schedule=" << schedule.name << "\n";
            return 0;
        }
        if (argc == 7 && std::string(argv[1]) == "--grammar" &&
            std::string(argv[2]) == "maps" &&
            std::string(argv[3]) == "--seed" &&
            std::string(argv[5]) == "--mutant") {
            const auto seed = fuzz::parse_seed(argv[4]);
            const auto mutant =
                static_cast<std::size_t>(fuzz::parse_seed(argv[6]));
            require_map_mutant_rejected(seed, mutant);
            std::cerr << "[PASS] maps source mutant replay seed=" << seed
                      << " mutant=" << mutant << "\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--dump-corpus" &&
            std::string(argv[2]) == "maps") {
            for (std::uint64_t seed = 1; seed <= 10; ++seed) {
                std::cout << "grammar=maps seed=" << seed << "\n"
                          << generate_maps_source(seed);
            }
            return 0;
        }
        if (argc != 1) {
            std::cerr << "usage: " << argv[0]
                      << " [--grammar maps --seed <uint64> --schedule <name>]\n"
                      << "       " << argv[0]
                      << " --grammar maps --seed <uint64> --mutant <index>\n"
                      << "       " << argv[0]
                      << " --dump-corpus maps\n";
            return 2;
        }
        const std::vector<Test> tests{
            {"heap_map_preserves_insertion_order_and_updates_in_place",
             heap_map_preserves_insertion_order_and_updates_in_place},
            {"old_map_barrier_keeps_young_string_key_and_ref_value_alive",
             old_map_barrier_keeps_young_string_key_and_ref_value_alive},
            {"skipped_map_barrier_is_detected_loudly",
             skipped_map_barrier_is_detected_loudly},
            {"structural_string_lookup_survives_full_compaction",
             structural_string_lookup_survives_full_compaction},
            {"descriptor_precision_keeps_scalar_bits_opaque_and_forwards_ref_slots",
             descriptor_precision_keeps_scalar_bits_opaque_and_forwards_ref_slots},
            {"growth_remains_exact_across_repeated_collections",
             growth_remains_exact_across_repeated_collections},
            {"update_collects_old_value_and_barriers_new_young_value",
             update_collects_old_value_and_barriers_new_young_value},
            {"opcodes_execute_set_get_has_and_len",
             opcodes_execute_set_get_has_and_len},
            {"verifier_reports_stable_map_reason_codes",
             verifier_reports_stable_map_reason_codes},
            {"nested_map_signature_facts_recover_value_type",
             nested_map_signature_facts_recover_value_type},
            {"missing_map_key_traps_with_stable_diagnostic",
             missing_map_key_traps_with_stable_diagnostic},
            {"vm_growth_survives_gc_every_instruction",
             vm_growth_survives_gc_every_instruction},
            {"frontend_surface_lowers_to_precise_layouts",
             frontend_surface_lowers_to_precise_layouts},
            {"frontend_reports_positioned_map_misuse",
             frontend_reports_positioned_map_misuse},
            {"frontend_structural_keys_and_nil_values_survive_stress",
             frontend_structural_keys_and_nil_values_survive_stress},
            {"maps_compose_with_arrays_pairs_closures_and_functions",
             maps_compose_with_arrays_pairs_closures_and_functions},
            {"canonical_oracle_renders_maps_as_ordered_key_value_lists",
             canonical_oracle_renders_maps_as_ordered_key_value_lists},
            {"maps_source_fuzz_pinned_snapshot",
             maps_source_fuzz_pinned_snapshot},
            {"maps_source_fuzz_corpus_and_mutants",
             maps_source_fuzz_corpus_and_mutants},
        };
        for (const auto& [name, test] : tests) {
            test();
            std::cerr << "[PASS] " << name << "\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
