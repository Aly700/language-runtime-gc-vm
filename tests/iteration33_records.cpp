#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr lang::ObjectId kSlotMask = 0xFFFF'FFFFull;

std::uint32_t slot_of(lang::ObjectId id) {
    return static_cast<std::uint32_t>(id & kSlotMask);
}

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

template <typename Fn>
void require_throws(Fn&& fn, const std::string& message) {
    try {
        fn();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}

struct VectorRoots final : lang::gc::RootProvider {
    std::vector<lang::Value> roots;

    void trace_roots(lang::gc::RootVisitor& visitor) override {
        for (auto& root : roots) {
            visitor.visit(root);
        }
    }
};

lang::gc::StressConfig maximum_stress() {
    lang::gc::StressConfig stress;
    stress.collect_before_every_allocation = true;
    stress.collect_after_every_allocation = true;
    stress.collect_every_n_instructions = 1;
    stress.collect_minor_every_n_instructions = 1;
    stress.collect_minor_after_every_write_barrier = true;
    return stress;
}

void interleaved_record_bitmap_keeps_scalar_object_id_bits_opaque() {
    lang::gc::Heap heap;
    const auto dead_a = heap.allocate_pair(lang::Value::int64(-1),
                                           lang::Value::int64(-2));
    const auto dead_b = heap.allocate_pair(lang::Value::int64(-3),
                                           lang::Value::int64(-4));
    const auto live_a = heap.allocate_pair(lang::Value::int64(10),
                                           lang::Value::int64(11));
    const auto live_b = heap.allocate_pair(lang::Value::int64(20),
                                           lang::Value::int64(21));

    heap.set_stress_config(maximum_stress());
    const auto record = heap.allocate_record(
        7,
        {lang::Value::object(live_a),
         lang::Value::int64(static_cast<std::int64_t>(dead_a)),
         lang::Value::object(live_b),
         lang::Value::int64(static_cast<std::int64_t>(dead_b))},
        {true, false, true, false});

    VectorRoots roots;
    roots.roots = {lang::Value::object(record)};
    heap.set_root_provider(&roots);
    heap.collect();

    const auto moved_record = roots.roots.at(0).as_object();
    require(heap.live_count() == 3,
            "scalar record fields kept dead ObjectIds live");
    require(heap.record_layout_index(moved_record) == 7 &&
                heap.record_field_count(moved_record) == 4,
            "record layout identity or fixed field count changed");
    require(heap.record_get(moved_record, 1).as_i64() ==
                static_cast<std::int64_t>(dead_a) &&
                heap.record_get(moved_record, 3).as_i64() ==
                static_cast<std::int64_t>(dead_b),
            "scalar record fields were interpreted or rewritten as references");
    require(heap.left(heap.record_get(moved_record, 0).as_object()).as_i64() == 10 &&
                heap.left(heap.record_get(moved_record, 2).as_object()).as_i64() == 20,
            "bitmap-selected record references were not retained and forwarded");
    require_throws([&] { (void)heap.object(dead_a); },
                   "dead ObjectId in scalar field remained live");
    require_throws([&] { (void)heap.object(dead_b); },
                   "second dead ObjectId in scalar field remained live");
}

void old_record_store_barriers_before_publish_and_validator_is_nonvacuous() {
    lang::gc::Heap heap;
    VectorRoots roots;
    const auto record = heap.allocate_record(
        0, {lang::Value::nil(), lang::Value::int64(1)}, {true, false});
    roots.roots = {lang::Value::object(record)};
    heap.set_root_provider(&roots);
    heap.collect();

    const auto old_record = roots.roots.at(0).as_object();
    require(heap.TEST_ONLY_is_old_object(old_record),
            "record did not promote before barrier test");
    const auto young = heap.allocate_pair(lang::Value::int64(41),
                                          lang::Value::int64(42));
    const auto barriers_before = heap.metrics().write_barrier_hits;
    heap.record_set(old_record, 0, lang::Value::object(young));
    require(heap.TEST_ONLY_remembered_set_size() == 1 &&
                heap.metrics().write_barrier_hits == barriers_before + 1,
            "old record store did not record the barrier before publication");
    heap.collect_minor();
    const auto moved_record = roots.roots.at(0).as_object();
    const auto moved_young = heap.record_get(moved_record, 0).as_object();
    require(heap.left(moved_young).as_i64() == 41,
            "minor collection lost the young record field");

    const auto next_young = heap.allocate_pair(lang::Value::int64(51),
                                               lang::Value::int64(52));
    heap.TEST_ONLY_skip_next_write_barrier_for_barrier_validator();
    heap.record_set(moved_record, 0, lang::Value::object(next_young));
    require_throws([&] { heap.TEST_ONLY_validate_gc_invariants(); },
                   "record remembered-set validator did not detect a skipped barrier");
}

void record_storage_width_and_promotion_edges_are_descriptor_owned() {
    lang::gc::Heap heap;
    const auto dead = heap.allocate_record(
        0,
        {lang::Value::int64(1), lang::Value::int64(2),
         lang::Value::int64(3), lang::Value::int64(4)},
        {false, false, false, false});
    const auto live = heap.allocate_record(
        1, {lang::Value::int64(42)}, {false});
    const auto tail = heap.allocate_pair(lang::Value::int64(5),
                                         lang::Value::int64(6));
    VectorRoots roots;
    roots.roots = {lang::Value::object(live), lang::Value::object(tail)};
    heap.collect(roots);
    require(slot_of(roots.roots.at(0).as_object()) == 0 &&
                slot_of(roots.roots.at(1).as_object()) == 2,
            "record compaction cursor did not use 1 + field_count width");
    (void)dead;

    lang::gc::Heap promotion_heap;
    const auto young = promotion_heap.allocate_pair(lang::Value::int64(7),
                                                     lang::Value::int64(8));
    const auto owner = promotion_heap.allocate_record(
        2, {lang::Value::object(young)}, {true});
    const auto barriers_before = promotion_heap.metrics().write_barrier_hits;
    promotion_heap.TEST_ONLY_promote_object_through_collector_path(owner);
    require(promotion_heap.TEST_ONLY_remembered_set_size() == 1 &&
                promotion_heap.metrics().write_barrier_hits == barriers_before,
            "promotion-created record edge was not recorded collector-internally");
}

lang::FunctionSignature make_function_signature(
    std::vector<lang::SignatureValue> parameters,
    lang::SignatureValue result) {
    lang::FunctionSignature signature;
    signature.parameter_types = parameters;
    for (const auto& parameter : parameters) {
        signature.parameters.push_back(parameter.kind);
    }
    signature.return_type = result.kind;
    signature.return_type_detail = std::move(result);
    return signature;
}

lang::RecordLayout cell_layout(std::string name = "Cell") {
    lang::RecordLayout layout;
    layout.name = std::move(name);
    layout.field_types = {
        lang::signature_value(lang::ValueKind::Int64),
        lang::pair_signature(
            lang::signature_value(lang::ValueKind::Int64),
            lang::signature_value(lang::ValueKind::Int64)),
    };
    layout.reference_map = {false, true};
    return layout;
}

lang::Module valid_record_module() {
    lang::Module module;
    module.entry_function = 0;
    module.record_layouts.push_back(cell_layout());

    lang::Function entry;
    entry.signature = make_function_signature(
        {}, lang::signature_value(lang::ValueKind::Int64));
    entry.local_count = 1;
    entry.code = {
        {lang::OpCode::ConstantI64, 7},
        {lang::OpCode::ConstantI64, 8},
        {lang::OpCode::ConstantI64, 9},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::AllocRecord, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::RecordGet, 0, 0},
        {lang::OpCode::Return, 0},
    };
    module.functions.push_back(std::move(entry));
    return module;
}

lang::VerifierReason first_reason(const lang::Module& module) {
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value() && !report.diagnostics.empty(),
            "malformed record module had no verifier diagnostic");
    return report.diagnostics.front().reason;
}

void verifier_accepts_records_with_exact_stack_maps_and_stable_reasons() {
    const auto valid = valid_record_module();
    const auto report = lang::verify_module_with_diagnostics(valid);
    require(report.module.has_value(),
            "verifier rejected a valid record module");
    require(report.module->verification().functions.at(0).stack_maps.at(4)
                .object_slots == std::vector<bool>({false, true}),
            "AllocRecord initializer stack map was not exact");
    require(report.module->verification().functions.at(0).stack_maps.at(7)
                .object_slots == std::vector<bool>({true}),
            "RecordGet receiver was not an exact stack-map root");

    {
        auto module = valid;
        module.record_layouts[0].reference_map[0] = true;
        require(first_reason(module) ==
                    lang::VerifierReason::BadRecordLayoutShape,
                "malformed record bitmap used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].code[4].operand = 9;
        require(first_reason(module) ==
                    lang::VerifierReason::BadRecordLayoutIndex,
                "bad AllocRecord layout used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].code[7].operand2 = 9;
        require(first_reason(module) ==
                    lang::VerifierReason::BadRecordFieldIndex,
                "bad RecordGet field index used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].code.erase(module.functions[0].code.begin() + 1,
                                       module.functions[0].code.begin() + 4);
        require(first_reason(module) ==
                    lang::VerifierReason::BadRecordInitializerArity,
                "missing record initializer used the wrong reason");
    }
    {
        auto module = valid;
        module.record_layouts[0].field_types[0] =
            lang::signature_value(lang::ValueKind::Bool);
        require(first_reason(module) ==
                    lang::VerifierReason::BadRecordInitializerType,
                "wrong record initializer type used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].local_count = 0;
        module.functions[0].code = {
            {lang::OpCode::ConstantI64, 1},
            {lang::OpCode::ConstantI64, 2},
            {lang::OpCode::AllocPair, 0},
            {lang::OpCode::RecordGet, 0, 0},
            {lang::OpCode::Return, 0},
        };
        require(first_reason(module) ==
                    lang::VerifierReason::RecordOperationOnNonRecord,
                "RecordGet on pair used the wrong reason");
    }
    {
        auto module = valid;
        module.record_layouts.push_back(cell_layout("OtherCell"));
        module.functions[0].code[7].operand = 1;
        require(first_reason(module) ==
                    lang::VerifierReason::RecordLayoutMismatch,
                "cross-layout record access used the wrong reason");
    }
    {
        lang::Module module;
        module.record_layouts.push_back(cell_layout());
        lang::Function function;
        function.signature = make_function_signature(
            {lang::record_signature(0)},
            lang::signature_value(lang::ValueKind::Int64));
        function.local_count = 1;
        function.code = {
            {lang::OpCode::LoadLocal, 0},
            {lang::OpCode::RecordGet, 0, 0},
            {lang::OpCode::Return, 0},
        };
        module.functions.push_back(std::move(function));
        require(first_reason(module) ==
                    lang::VerifierReason::RecordReceiverMayBeNil,
                "possibly-nil record access used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].code = {
            {lang::OpCode::ConstantI64, 7},
            {lang::OpCode::ConstantI64, 8},
            {lang::OpCode::ConstantI64, 9},
            {lang::OpCode::AllocPair, 0},
            {lang::OpCode::AllocRecord, 0},
            {lang::OpCode::StoreLocal, 0},
            {lang::OpCode::LoadLocal, 0},
            {lang::OpCode::ConstantI64, 0},
            {lang::OpCode::ConstantI64, 1},
            {lang::OpCode::LessI64, 0},
            {lang::OpCode::RecordSet, 0, 0},
            {lang::OpCode::ConstantI64, 0},
            {lang::OpCode::Return, 0},
        };
        require(first_reason(module) ==
                    lang::VerifierReason::RecordFieldTypeMismatch,
                "wrong record store type used the wrong reason");
    }
    require(std::string(lang::verifier_reason_name(
                lang::VerifierReason::BadRecordLayoutShape)) ==
                "BadRecordLayoutShape" &&
                std::string(lang::verifier_reason_name(
                    lang::VerifierReason::RecordFieldTypeMismatch)) ==
                    "RecordFieldTypeMismatch",
            "record verifier reason names are not stable");
}

lang::Module suspended_record_mutation_module() {
    lang::Module module;
    module.entry_function = 0;
    module.record_layouts.push_back(cell_layout());

    lang::Function entry;
    entry.signature = make_function_signature(
        {}, lang::signature_value(lang::ValueKind::Int64));
    entry.local_count = 2;
    entry.code = {
        {lang::OpCode::ConstantI64, -1},
        {lang::OpCode::ConstantI64, -2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::ConstantI64, 5},
        {lang::OpCode::ConstantI64, 10},
        {lang::OpCode::ConstantI64, 11},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::AllocRecord, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::Nil, 0},
        {lang::OpCode::StoreLocal, 1},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::Call, 1},
        {lang::OpCode::RecordSet, 0, 1},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::RecordGet, 0, 1},
        {lang::OpCode::GetLeft, 0},
        {lang::OpCode::Return, 0},
    };

    lang::Function helper;
    helper.signature = make_function_signature(
        {}, lang::pair_signature(
                lang::signature_value(lang::ValueKind::Int64),
                lang::signature_value(lang::ValueKind::Int64)));
    helper.code = {
        {lang::OpCode::ConstantI64, 41},
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Collect, 0},
        {lang::OpCode::Return, 0},
    };
    module.functions = {std::move(entry), std::move(helper)};
    return module;
}

void suspended_frames_forward_records_before_resuming_mutation() {
    auto report = lang::verify_module_with_diagnostics(
        suspended_record_mutation_module());
    require(report.module.has_value(),
            "verifier rejected suspended-record compaction module");
    lang::VM vm;
    vm.set_gc_stress(maximum_stress());
    require(vm.execute(*report.module).as_i64() == 41,
            "record mutation resumed through a stale suspended-frame owner");
    require(vm.metrics().heap.objects_moved > 0,
            "suspended-frame record test did not exercise movement");
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main(int argc, char** argv) {
    const std::vector<TestCase> tests = {
        {"interleaved_record_bitmap_keeps_scalar_object_id_bits_opaque",
         interleaved_record_bitmap_keeps_scalar_object_id_bits_opaque},
        {"old_record_store_barriers_before_publish_and_validator_is_nonvacuous",
         old_record_store_barriers_before_publish_and_validator_is_nonvacuous},
        {"record_storage_width_and_promotion_edges_are_descriptor_owned",
         record_storage_width_and_promotion_edges_are_descriptor_owned},
        {"verifier_accepts_records_with_exact_stack_maps_and_stable_reasons",
         verifier_accepts_records_with_exact_stack_maps_and_stable_reasons},
        {"suspended_frames_forward_records_before_resuming_mutation",
         suspended_frames_forward_records_before_resuming_mutation},
    };

    std::string selected;
    if (argc == 3 && std::string(argv[1]) == "--test") {
        selected = argv[2];
    } else if (argc != 1) {
        std::cerr << "usage: " << argv[0] << " [--test NAME]\n";
        return 2;
    }

    int failures = 0;
    bool ran = false;
    for (const auto& test : tests) {
        if (!selected.empty() && selected != test.name) {
            continue;
        }
        ran = true;
        try {
            test.run();
            std::cerr << "[PASS] " << test.name << "\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << "\n"
                      << error.what() << "\n";
        }
    }
    if (!ran) {
        std::cerr << "unknown iteration-33 record test: " << selected << "\n";
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " iteration-33 record test(s) failed\n";
        return 1;
    }
    return 0;
}
