#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "fuzz_common.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <span>
#include <utility>
#include <vector>

namespace {

using lang::Value;
using lang::gc::Heap;

lang::ObjectId allocate_string(Heap& heap, std::string_view text) {
    return heap.allocate_string(std::span<const std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
}

std::string string_value(const Heap& heap, lang::ObjectId id) {
    const auto bytes = heap.string_bytes(id);
    return std::string(bytes.begin(), bytes.end());
}

std::string builder_value(const Heap& heap, lang::ObjectId id) {
    const auto bytes = heap.builder_bytes(id);
    return std::string(bytes.begin(), bytes.end());
}

bool is_stale_id(const Heap& heap, lang::ObjectId id) {
    try {
        (void)heap.object(id);
    } catch (const std::out_of_range&) {
        return true;
    }
    return false;
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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

lang::frontend::CompileResult require_compiles(const std::string& source) {
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(),
            "expected Builder source to compile\n" + source + "\n" +
                diagnostics_listing(compiled.diagnostics));
    return compiled;
}

void require_diagnostic(const std::string& source, std::size_t line,
                        std::size_t column, const std::string& message) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok(), "expected Builder source rejection\n" + source);
    for (const auto& diagnostic : compiled.diagnostics) {
        if (diagnostic.position.line == line &&
            diagnostic.position.column == column &&
            diagnostic.message.find(message) != std::string::npos) {
            return;
        }
    }
    throw std::runtime_error(
        "missing diagnostic containing '" + message + "' at " +
        std::to_string(line) + ":" + std::to_string(column) + "\n" +
        diagnostics_listing(compiled.diagnostics));
}

void source_surface_accepts_string_scalar_append_snapshot_len_and_clear() {
    require_compiles(R"SRC(let b: builder = builder();
b.append("prefix");
b.append(42);
b.append(true);
let snapshot: str = b.to_str();
let before_clear: i64 = b.len;
b.clear();
b.append(false);
print(snapshot);
print(b.to_str());
before_clear
)SRC");
}

void source_surface_rejects_wrong_receiver_arity_and_append_type() {
    require_diagnostic(R"SRC(let n: i64 = 1;
n.append("x");
0
)SRC",
                       2, 1, "append requires builder receiver");

    require_diagnostic(R"SRC(let b: builder = builder();
b.append();
0
)SRC",
                       2, 1, "append expects exactly 1 argument");

    require_diagnostic(R"SRC(let b: builder = builder();
let other: builder = builder();
b.append(other);
0
)SRC",
                       3, 10, "builder append accepts str, i64, or bool");
}

void source_execution_is_exact_across_all_collection_schedules() {
    const auto compiled = require_compiles(R"SRC(let b: builder = builder();
b.append("prefix");
b.append(42);
b.append(true);
let snapshot: str = b.to_str();
let before_clear: i64 = b.len;
b.clear();
b.append(false);
print(snapshot);
print(b.to_str());
before_clear
)SRC");

    const auto schedules = fuzz::schedules();
    require(schedules.size() == 15,
            "Builder schedule matrix no longer contains all 15 schedules");
    for (const auto& schedule : schedules) {
        const auto outcome =
            fuzz::execute_once(*compiled.verified_module, schedule);
        require(outcome.ok,
                std::string("Builder source trapped under ") + schedule.name +
                    ": " + outcome.error);
        require(outcome.observable == "i64:12",
                std::string("Builder length drifted under ") + schedule.name +
                    ": " + outcome.observable);
        require(outcome.output == "prefix42true\nfalse\n",
                std::string("Builder output drifted under ") + schedule.name +
                    "\n" + fuzz::render_output_bytes(outcome.output));
    }
}

void bytecode_is_append_only_and_stack_maps_are_exact() {
    require(static_cast<std::size_t>(lang::OpCode::AllocBuilder) ==
                static_cast<std::size_t>(lang::OpCode::I64Abs) + 1 &&
                static_cast<std::size_t>(lang::OpCode::BuilderAppend) ==
                    static_cast<std::size_t>(
                        lang::OpCode::AllocBuilder) +
                        1 &&
                static_cast<std::size_t>(lang::OpCode::BuilderLen) ==
                    static_cast<std::size_t>(
                        lang::OpCode::BuilderAppend) +
                        1 &&
                static_cast<std::size_t>(lang::OpCode::BuilderToStr) ==
                    static_cast<std::size_t>(lang::OpCode::BuilderLen) + 1 &&
                static_cast<std::size_t>(lang::OpCode::BuilderClear) ==
                    static_cast<std::size_t>(
                        lang::OpCode::BuilderToStr) +
                        1,
            "Builder opcodes were not appended in their documented order");
    require(
        static_cast<std::size_t>(
            lang::VerifierReason::BuilderOperationOnNonBuilder) ==
                static_cast<std::size_t>(
                    lang::VerifierReason::I64AbsRequiresI64) +
                    1 &&
            static_cast<std::size_t>(
                lang::VerifierReason::BuilderAppendRequiresStr) ==
                static_cast<std::size_t>(
                    lang::VerifierReason::BuilderOperationOnNonBuilder) +
                    1,
        "Builder verifier reasons were not appended in stable order");

    lang::Module module;
    module.string_constants = {"x"};
    lang::Function entry;
    entry.signature.return_type = lang::ValueKind::Int64;
    entry.local_count = 1;
    entry.code = {
        {lang::OpCode::AllocBuilder, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::PushStr, 0},
        {lang::OpCode::BuilderAppend, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::BuilderToStr, 0},
        {lang::OpCode::Print, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::BuilderClear, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::BuilderLen, 0},
        {lang::OpCode::Return, 0},
    };
    module.functions.push_back(std::move(entry));

    const auto report =
        lang::verify_module_with_diagnostics(std::move(module));
    require(report.module.has_value(),
            "verifier rejected valid Builder bytecode: " +
                (report.diagnostics.empty()
                     ? std::string("<no diagnostic>")
                     : lang::format_verifier_diagnostic(
                           report.diagnostics.front())));
    const auto& maps =
        report.module->verification().functions.front().stack_maps;
    const std::vector<std::vector<bool>> expected_stack_maps{
        {},
        {true},
        {},
        {true},
        {true, true},
        {},
        {true},
        {true},
        {},
        {true},
        {},
        {true},
        {false},
    };
    require(maps.size() == expected_stack_maps.size(),
            "Builder verifier emitted wrong stack-map count");
    for (std::size_t pc = 0; pc < maps.size(); ++pc) {
        require(maps[pc].object_slots == expected_stack_maps[pc],
                "Builder stack map mismatch at pc " +
                    std::to_string(pc));
        require(maps[pc].local_object_slots ==
                    std::vector<bool>{pc < 2 ? false : true},
                "Builder local root map mismatch at pc " +
                    std::to_string(pc));
    }
}

void bytecode_rejections_use_stable_builder_reasons() {
    lang::Function wrong_receiver;
    wrong_receiver.signature.return_type = lang::ValueKind::Int64;
    wrong_receiver.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::BuilderLen, 0},
        {lang::OpCode::Return, 0},
    };
    auto report = lang::verify_with_diagnostics(wrong_receiver);
    require(!report.result.has_value() && !report.diagnostics.empty() &&
                report.diagnostics.front().reason ==
                    lang::VerifierReason::BuilderOperationOnNonBuilder,
            "non-Builder receiver used the wrong verifier reason");

    lang::Function wrong_append;
    wrong_append.signature.return_type = lang::ValueKind::Int64;
    wrong_append.code = {
        {lang::OpCode::AllocBuilder, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::BuilderAppend, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::Return, 0},
    };
    report = lang::verify_with_diagnostics(wrong_append);
    require(!report.result.has_value() && !report.diagnostics.empty() &&
                report.diagnostics.front().reason ==
                    lang::VerifierReason::BuilderAppendRequiresStr,
            "non-Str Builder append used the wrong verifier reason");
}

void source_lowering_uses_one_append_opcode_and_existing_conversions() {
    const auto compiled = require_compiles(R"SRC(let b: builder = builder();
b.append("x");
b.append(7);
b.append(false);
let snapshot: str = b.to_str();
let length: i64 = b.len;
b.clear();
length
)SRC");
    const auto& code =
        compiled.verified_module->module().functions.front().code;
    std::size_t alloc_count = 0;
    std::size_t append_count = 0;
    std::size_t len_count = 0;
    std::size_t snapshot_count = 0;
    std::size_t clear_count = 0;
    std::vector<lang::OpCode> append_predecessors;
    for (std::size_t pc = 0; pc < code.size(); ++pc) {
        switch (code[pc].op) {
        case lang::OpCode::AllocBuilder:
            ++alloc_count;
            break;
        case lang::OpCode::BuilderAppend:
            ++append_count;
            require(pc != 0,
                    "BuilderAppend cannot be the first instruction");
            append_predecessors.push_back(code[pc - 1].op);
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
    require(alloc_count == 1 && append_count == 3 && len_count == 1 &&
                snapshot_count == 1 && clear_count == 1,
            "source lowering omitted or duplicated a Builder opcode");
    require(append_predecessors ==
                std::vector<lang::OpCode>{
                    lang::OpCode::PushStr,
                    lang::OpCode::I64ToStr,
                    lang::OpCode::BoolToStr,
                },
            "Builder scalar append did not reuse the existing to_str lowering");
}

void heap_builder_has_opaque_empty_shape_and_deterministic_capacity() {
    Heap heap;
    const auto builder = heap.allocate_builder();
    require(heap.TEST_ONLY_is_builder(builder),
            "allocate_builder did not produce ObjectKind::Builder");
    require(heap.builder_length(builder) == 0,
            "fresh Builder length was not zero");
    require(heap.builder_bytes(builder).empty(),
            "fresh Builder contained payload bytes");
    require(heap.TEST_ONLY_builder_capacity(builder) == 8,
            "fresh Builder capacity was not the deterministic eight bytes");
    require(heap.capacity_slots() == 2,
            "fresh eight-byte Builder did not reserve exactly two slots");
    heap.TEST_ONLY_validate_gc_invariants();
}

void builder_shape_corruption_fails_loudly() {
    Heap heap;
    const auto builder = heap.allocate_builder();
    heap.TEST_ONLY_corrupt_builder_shape(builder);
    try {
        heap.TEST_ONLY_validate_gc_invariants();
    } catch (const std::logic_error& error) {
        require(
            std::string(error.what()) ==
                "builder byte payload length disagrees with logical length",
            "Builder shape corruption produced the wrong failure: " +
                std::string(error.what()));
        return;
    }
    throw std::runtime_error(
        "Builder shape corruption passed descriptor validation");
}

void canonical_graph_renders_logical_builder_bytes_without_capacity() {
    Heap heap;
    const auto builder = heap.allocate_builder();
    require(fuzz::canonical_object_graph(heap, builder) ==
                "object(@0)\n  @0 = builder[0]()",
            "canonical Builder graph exposed capacity or omitted logical bytes");
}

void fixed_width_mutations_clear_reuse_and_snapshots_are_independent() {
    Heap heap;
    auto builder = heap.make_handle(heap.allocate_builder());
    auto empty = heap.make_handle(allocate_string(heap, ""));
    auto abc = heap.make_handle(allocate_string(heap, "abc"));
    auto def = heap.make_handle(allocate_string(heap, "def"));
    auto z = heap.make_handle(allocate_string(heap, "z"));

    heap.builder_append(builder.object(), abc.value());
    heap.builder_append(builder.object(), empty.value());
    auto first =
        heap.make_handle(heap.builder_to_string(builder.value()));
    heap.builder_append(builder.object(), def.value());
    auto second =
        heap.make_handle(heap.builder_to_string(builder.value()));

    require(string_value(heap, first.object()) == "abc" &&
                string_value(heap, second.object()) == "abcdef",
            "Builder snapshots did not copy the bytes present at conversion");
    require(first.object() != second.object(),
            "to_str reused an immutable snapshot object");

    heap.builder_clear(builder.object());
    require(heap.builder_length(builder.object()) == 0 &&
                heap.TEST_ONLY_builder_capacity(builder.object()) == 8,
            "clear did not retain identity/capacity while resetting length");
    heap.builder_append(builder.object(), z.value());
    require(std::string(heap.builder_bytes(builder.object()).begin(),
                        heap.builder_bytes(builder.object()).end()) == "z",
            "Builder was not reusable after clear");
    require(string_value(heap, first.object()) == "abc" &&
                string_value(heap, second.object()) == "abcdef",
            "later Builder mutation changed an immutable snapshot");

    heap.builder_clear(builder.object());
    auto empty_first =
        heap.make_handle(heap.builder_to_string(builder.value()));
    auto empty_second =
        heap.make_handle(heap.builder_to_string(builder.value()));
    require(empty_first.object() != empty_second.object() &&
                string_value(heap, empty_first.object()).empty() &&
                string_value(heap, empty_second.object()).empty(),
            "empty to_str did not allocate independent immutable strings");
    heap.TEST_ONLY_validate_gc_invariants();
}

void fixed_width_mutation_keeps_incremental_compaction_active_and_in_sync() {
    Heap heap;
    auto builder = heap.make_handle(heap.allocate_builder());
    auto abc = heap.make_handle(allocate_string(heap, "abc"));
    auto x = heap.make_handle(allocate_string(heap, "x"));

    heap.start_incremental_compaction();
    require(heap.incremental_compaction_active(),
            "incremental compaction did not start");
    heap.builder_append(builder.object(), abc.value());
    require(heap.incremental_compaction_active(),
            "fixed-width append unnecessarily finished compaction");
    heap.builder_clear(builder.object());
    require(heap.incremental_compaction_active(),
            "clear unnecessarily finished compaction");
    heap.builder_append(builder.object(), x.value());
    require(heap.incremental_compaction_active(),
            "fixed-width reuse unnecessarily finished compaction");
    heap.finish_incremental_compaction();

    require(std::string(heap.builder_bytes(builder.object()).begin(),
                        heap.builder_bytes(builder.object()).end()) == "x",
            "incremental-compaction shadow lost fixed-width Builder mutation");
    heap.TEST_ONLY_validate_gc_invariants();
}

void deterministic_growth_transitions_extend_in_place_at_heap_end() {
    Heap heap;
    auto nine = heap.make_handle(allocate_string(heap, "123456789"));
    auto eight = heap.make_handle(allocate_string(heap, "abcdefgh"));
    auto builder = heap.make_handle(heap.allocate_builder());
    const auto original = builder.object();
    const auto initial_slots = heap.capacity_slots();

    heap.builder_append(builder.object(), nine.value());
    require(builder.object() == original &&
                heap.TEST_ONLY_builder_capacity(builder.object()) == 16 &&
                heap.capacity_slots() == initial_slots + 1,
            "8->16 Builder growth did not extend an adjacent heap-end run");

    heap.builder_append(builder.object(), eight.value());
    require(builder.object() == original &&
                heap.TEST_ONLY_builder_capacity(builder.object()) == 32 &&
                heap.capacity_slots() == initial_slots + 3 &&
                builder_value(heap, builder.object()) ==
                    "123456789abcdefgh",
            "16->32 Builder growth violated the deterministic ladder or bytes");
    heap.TEST_ONLY_validate_gc_invariants();
}

void blocked_growth_relocates_forwards_handle_and_reclaims_old_run() {
    Heap heap;
    auto builder = heap.make_handle(heap.allocate_builder());
    const auto old_id = builder.object();
    auto blocker = heap.make_handle(
        heap.allocate_pair(Value::int64(1), Value::int64(2)));
    auto nine = heap.make_handle(allocate_string(heap, "123456789"));
    const auto moved_before = heap.metrics().objects_moved;

    heap.builder_append(builder.object(), nine.value());

    require(builder.object() != old_id &&
                heap.TEST_ONLY_builder_capacity(builder.object()) == 16 &&
                builder_value(heap, builder.object()) == "123456789",
            "blocked Builder growth did not relocate content and identity");
    require(is_stale_id(heap, old_id),
            "blocked Builder growth left the old complete ObjectId usable");
    require(heap.metrics().objects_moved == moved_before + 1,
            "blocked Builder growth did not record exactly one moved object");

    const auto replacement =
        heap.allocate_pair(Value::int64(3), Value::int64(4));
    constexpr lang::ObjectId slot_mask = 0xFFFF'FFFFull;
    require((replacement & slot_mask) == (old_id & slot_mask) &&
                replacement != old_id,
            "relocated Builder's old storage run was not reclaimed");
    (void)blocker;
    heap.TEST_ONLY_validate_gc_invariants();
}

void growth_finishes_compaction_with_source_as_precise_temporary_root() {
    Heap heap;
    auto builder = heap.make_handle(heap.allocate_builder());
    (void)heap.allocate_pair(Value::int64(7), Value::int64(8));
    Value source = Value::nil();
    {
        auto source_handle =
            heap.make_handle(allocate_string(heap, "rooted-9!"));
        source = source_handle.value();
        heap.start_incremental_compaction();
    }
    require(heap.incremental_compaction_active(),
            "growth-root test did not enter incremental compaction");

    const auto old_id = builder.object();
    heap.builder_append(builder.object(), source);

    require(!heap.incremental_compaction_active(),
            "width-changing append did not finish compaction first");
    require(builder_value(heap, builder.object()) == "rooted-9!" &&
                heap.TEST_ONLY_builder_capacity(builder.object()) == 16,
            "growth collection lost the append source or Builder bytes");
    require(builder.object() != old_id && is_stale_id(heap, old_id),
            "post-compaction blocked growth did not forward Builder identity");
    heap.TEST_ONLY_validate_gc_invariants();
}

void opaque_builder_bytes_never_trace_dead_object_id_bit_patterns() {
    Heap heap;
    auto builder = heap.make_handle(heap.allocate_builder());
    const auto dead =
        heap.allocate_pair(Value::int64(11), Value::int64(22));
    std::vector<std::uint8_t> alias(sizeof(lang::ObjectId));
    for (std::size_t i = 0; i < alias.size(); ++i) {
        alias[i] = static_cast<std::uint8_t>(
            (dead >> (i * 8)) & 0xffu);
    }
    {
        auto source = heap.make_handle(heap.allocate_string(alias));
        heap.builder_append(builder.object(), source.value());
    }

    heap.collect_minor();
    require(is_stale_id(heap, dead),
            "minor GC traced ObjectId-shaped Builder payload bytes");
    require(std::vector<std::uint8_t>(
                heap.builder_bytes(builder.object()).begin(),
                heap.builder_bytes(builder.object()).end()) == alias,
            "minor GC rewrote opaque Builder payload bytes");
    heap.collect();
    require(std::vector<std::uint8_t>(
                heap.builder_bytes(builder.object()).begin(),
                heap.builder_bytes(builder.object()).end()) == alias,
            "major GC traced or forwarded ObjectId-shaped Builder bytes");
    heap.TEST_ONLY_validate_gc_invariants();
}

void builder_in_record_capture_and_map_survives_incremental_compaction() {
    Heap heap;
    (void)heap.allocate_pair(Value::int64(-1), Value::int64(-2));

    lang::ObjectId old_builder = 0;
    {
        auto source =
            heap.make_handle(allocate_string(heap, "container-safe"));
        old_builder = heap.allocate_builder();
        heap.builder_append(old_builder, source.value());
    }

    auto record = heap.make_handle(heap.allocate_record(
        47, {Value::object(old_builder)}, {true}));
    auto closure = heap.make_handle(heap.allocate_closure(
        47, 0, {Value::object(old_builder)}, {true}));
    auto map =
        heap.make_handle(heap.allocate_map(47, false, true));
    heap.map_set(map.object(), Value::int64(47),
                 Value::object(old_builder));

    heap.start_incremental_compaction();
    require(heap.incremental_compact_step(1) == 1 &&
                heap.incremental_compaction_active(),
            "container crown did not pause incremental compaction mid-phase");
    while (!heap.incremental_compaction_quiescent()) {
        require(heap.incremental_compact_step(1) == 1,
                "container crown failed to advance incremental compaction");
    }
    heap.finish_incremental_compaction();

    const auto from_record =
        heap.record_get(record.object(), 0).as_object();
    const auto from_capture =
        heap.closure_capture(closure.object(), 0).as_object();
    const auto from_map =
        heap.map_get(map.object(), Value::int64(47)).as_object();
    require(from_record == from_capture && from_record == from_map,
            "record, closure, and map disagree on forwarded Builder identity");
    require(from_record != old_builder && is_stale_id(heap, old_builder),
            "incremental compaction did not move and stale the old Builder id");
    require(builder_value(heap, from_record) == "container-safe",
            "container-held Builder lost its opaque bytes during compaction");
    heap.TEST_ONLY_validate_gc_invariants();
}

void weak_builder_clears_when_no_strong_reference_survives() {
    Heap heap;
    const auto builder = heap.allocate_builder();
    auto weak = heap.make_handle(
        heap.allocate_weak(Value::object(builder)));

    heap.collect();

    require(weak.value().is_object() &&
                heap.weak_get(weak.object()).tag() == Value::Tag::Nil,
            "weak<builder> did not clear after its target became unreachable");
    require(is_stale_id(heap, builder),
            "weak<builder> kept its target strongly alive");
    heap.TEST_ONLY_validate_gc_invariants();
}

using Test = std::pair<const char*, void (*)()>;

} // namespace

int main() {
    const std::vector<Test> tests{
        {"bytecode_is_append_only_and_stack_maps_are_exact",
         bytecode_is_append_only_and_stack_maps_are_exact},
        {"bytecode_rejections_use_stable_builder_reasons",
         bytecode_rejections_use_stable_builder_reasons},
        {"source_lowering_uses_one_append_opcode_and_existing_conversions",
         source_lowering_uses_one_append_opcode_and_existing_conversions},
        {"source_surface_accepts_string_scalar_append_snapshot_len_and_clear",
         source_surface_accepts_string_scalar_append_snapshot_len_and_clear},
        {"source_surface_rejects_wrong_receiver_arity_and_append_type",
         source_surface_rejects_wrong_receiver_arity_and_append_type},
        {"source_execution_is_exact_across_all_collection_schedules",
         source_execution_is_exact_across_all_collection_schedules},
        {"heap_builder_has_opaque_empty_shape_and_deterministic_capacity",
         heap_builder_has_opaque_empty_shape_and_deterministic_capacity},
        {"builder_shape_corruption_fails_loudly",
         builder_shape_corruption_fails_loudly},
        {"canonical_graph_renders_logical_builder_bytes_without_capacity",
         canonical_graph_renders_logical_builder_bytes_without_capacity},
        {"fixed_width_mutations_clear_reuse_and_snapshots_are_independent",
         fixed_width_mutations_clear_reuse_and_snapshots_are_independent},
        {"fixed_width_mutation_keeps_incremental_compaction_active_and_in_sync",
         fixed_width_mutation_keeps_incremental_compaction_active_and_in_sync},
        {"deterministic_growth_transitions_extend_in_place_at_heap_end",
         deterministic_growth_transitions_extend_in_place_at_heap_end},
        {"blocked_growth_relocates_forwards_handle_and_reclaims_old_run",
         blocked_growth_relocates_forwards_handle_and_reclaims_old_run},
        {"growth_finishes_compaction_with_source_as_precise_temporary_root",
         growth_finishes_compaction_with_source_as_precise_temporary_root},
        {"opaque_builder_bytes_never_trace_dead_object_id_bit_patterns",
         opaque_builder_bytes_never_trace_dead_object_id_bit_patterns},
        {"builder_in_record_capture_and_map_survives_incremental_compaction",
         builder_in_record_capture_and_map_survives_incremental_compaction},
        {"weak_builder_clears_when_no_strong_reference_survives",
         weak_builder_clears_when_no_strong_reference_survives},
    };
    try {
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
