#include "fuzz_common.hpp"

#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
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
void require_throws(Fn&& fn, std::string_view expected,
                    const std::string& message) {
    try {
        fn();
    } catch (const std::exception& error) {
        require(std::string_view(error.what()).find(expected) !=
                    std::string_view::npos,
                message + ": got '" + error.what() + "'");
        return;
    }
    throw std::runtime_error(message + ": no exception");
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

lang::VariantLayout choice_layout() {
    lang::VariantCaseLayout left;
    left.name = "Left";
    left.field_types = {
        lang::pair_signature(lang::signature_value(lang::ValueKind::Int64),
                             lang::signature_value(lang::ValueKind::Int64)),
        lang::signature_value(lang::ValueKind::Int64),
    };
    left.reference_map = {true, false};

    lang::VariantCaseLayout right;
    right.name = "Right";
    right.field_types = {
        lang::signature_value(lang::ValueKind::Int64),
        lang::pair_signature(lang::signature_value(lang::ValueKind::Int64),
                             lang::signature_value(lang::ValueKind::Int64)),
    };
    right.reference_map = {false, true};

    lang::VariantLayout layout;
    layout.name = "Choice";
    layout.cases = {std::move(left), std::move(right)};
    return layout;
}

void per_case_bitmap_precision() {
    const auto raw = lang::gc::Object::variant(
        7, 0, {lang::Value::int64(1)}, {{false}, {true}});
    require(raw.kind == lang::gc::ObjectKind::Variant,
            "Object::variant did not construct the append-only heap kind");

    lang::gc::Heap heap;
    const auto dead_a = heap.allocate_pair(lang::Value::int64(-1),
                                           lang::Value::int64(-2));
    const auto dead_b = heap.allocate_pair(lang::Value::int64(-3),
                                           lang::Value::int64(-4));
    const auto live_a = heap.allocate_pair(lang::Value::int64(10),
                                           lang::Value::int64(11));
    const auto live_b = heap.allocate_pair(lang::Value::int64(20),
                                           lang::Value::int64(21));

    VectorRoots roots;
    roots.roots = {lang::Value::object(live_a), lang::Value::object(live_b)};
    heap.set_root_provider(&roots);
    heap.set_stress_config(maximum_stress());
    const std::vector<std::vector<bool>> case_maps = {
        {true, false}, {false, true}};

    const auto left = heap.allocate_variant(
        7, 0,
        {roots.roots.at(0),
         lang::Value::int64(static_cast<std::int64_t>(dead_a))},
        case_maps);
    roots.roots.push_back(lang::Value::object(left));
    const auto right = heap.allocate_variant(
        7, 1,
        {lang::Value::int64(static_cast<std::int64_t>(dead_b)),
         roots.roots.at(1)},
        case_maps);
    roots.roots.push_back(lang::Value::object(right));
    roots.roots.erase(roots.roots.begin(), roots.roots.begin() + 2);
    heap.collect();

    const auto moved_left = roots.roots.at(0).as_object();
    const auto moved_right = roots.roots.at(1).as_object();
    require(heap.live_count() == 4,
            "inactive/scalar variant slots retained dead ObjectIds");
    require(heap.TEST_ONLY_is_variant(moved_left) &&
                heap.object(moved_right).kind == lang::gc::ObjectKind::Variant,
            "variant kind did not survive compaction");
    require(heap.variant_layout_index(moved_left) == 7 &&
                heap.variant_tag(moved_left) == 0 &&
                heap.variant_tag(moved_right) == 1 &&
                heap.variant_field_count(moved_left) == 2,
            "variant nominal identity, tag, or active width changed");
    require(heap.variant_get(moved_left, 1).as_i64() ==
                static_cast<std::int64_t>(dead_a) &&
                heap.variant_get(moved_right, 0).as_i64() ==
                    static_cast<std::int64_t>(dead_b),
            "scalar variant fields were interpreted or forwarded as references");
    require(heap.left(heap.variant_get(moved_left, 0).as_object()).as_i64() == 10 &&
                heap.left(heap.variant_get(moved_right, 1).as_object()).as_i64() == 20,
            "selected variant reference fields were not retained and forwarded");
    require_throws([&] { (void)heap.object(dead_a); }, "stale",
                   "dead ObjectId in scalar variant field remained live");
    require_throws([&] { (void)heap.object(dead_b); }, "stale",
                   "second dead scalar ObjectId remained live");
}

lang::Module sibling_case_mismatch_module() {
    lang::Module module;
    module.entry_function = 0;
    module.variant_layouts.push_back(choice_layout());

    auto variant = lang::variant_signature(0);
    require(variant.kind == lang::ValueKind::Variant &&
                variant.variant_layout == std::optional<std::size_t>(0),
            "variant signature did not retain nominal layout identity");

    lang::Function entry;
    entry.signature = make_function_signature(
        {}, lang::signature_value(lang::ValueKind::Int64));
    entry.local_count = 1;
    entry.code = {
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::ConstantI64, 9},
        {lang::OpCode::AllocVariant, 0, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::VariantTag, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 14},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::VariantGet, 0, 1, 0},
        {lang::OpCode::Return, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::Return, 0},
    };
    require(entry.code.at(7).op == lang::OpCode::VariantTag &&
                entry.code.at(12).operand3 == 0,
            "append-only third instruction operand was not retained");
    module.functions.push_back(std::move(entry));
    return module;
}

lang::VariantLayout recursive_layout() {
    lang::VariantCaseLayout empty;
    empty.name = "Empty";

    lang::VariantCaseLayout branch;
    branch.name = "Branch";
    branch.field_types = {
        lang::array_signature(lang::variant_signature(1)),
        lang::map_signature(lang::signature_value(lang::ValueKind::Str),
                            lang::variant_signature(1)),
        lang::weak_signature(lang::variant_signature(1)),
        lang::function_signature({lang::variant_signature(1)},
                                 lang::variant_signature(1)),
        lang::pair_signature(lang::variant_signature(1),
                             lang::variant_signature(1)),
    };
    branch.reference_map = {true, true, true, true, true};

    lang::VariantLayout layout;
    layout.name = "Recursive";
    layout.cases = {std::move(empty), std::move(branch)};
    return layout;
}

lang::VerifierReason first_reason(const lang::Module& module) {
    const auto report = lang::verify_with_diagnostics(module);
    require(!report.result.has_value() && !report.diagnostics.empty(),
            "malformed variant module had no verifier diagnostic");
    return report.diagnostics.front().reason;
}

void backend_verifier_accepts_variants_and_rejects_malformed_modules() {
    auto valid = sibling_case_mismatch_module();
    valid.variant_layouts.push_back(recursive_layout());
    const auto report = lang::verify_module_with_diagnostics(valid);
    require(report.module.has_value(),
            "verifier rejected valid recursive variant signatures");
    const auto& maps = report.module->verification().functions.at(0).stack_maps;
    require(maps.at(4).object_slots == std::vector<bool>({true, false}),
            "AllocVariant initializer stack map was not exact");
    require(maps.at(7).object_slots == std::vector<bool>({true}) &&
                maps.at(12).object_slots == std::vector<bool>({true}) &&
                maps.at(13).object_slots == std::vector<bool>({false}),
            "variant receiver or selected scalar field stack map was not exact");

    {
        auto module = valid;
        module.variant_layouts[0].name.clear();
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutShape,
                "empty variant type name used the wrong reason");
    }
    {
        auto module = valid;
        module.variant_layouts[1].name = module.variant_layouts[0].name;
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutShape,
                "duplicate variant type name used the wrong reason");
    }
    {
        auto module = valid;
        module.variant_layouts[0].cases.clear();
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutShape,
                "variant with no cases used the wrong reason");
    }
    {
        auto module = valid;
        module.variant_layouts[0].cases[0].name.clear();
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutShape,
                "empty variant case name used the wrong reason");
    }
    {
        auto module = valid;
        module.variant_layouts[0].cases[1].name =
            module.variant_layouts[0].cases[0].name;
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutShape,
                "duplicate variant case name used the wrong reason");
    }
    {
        auto module = valid;
        module.variant_layouts[0].cases[0].field_types[0] =
            lang::array_signature(
                lang::signature_value(lang::ValueKind::Variant));
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutShape,
                "malformed nested variant signature used the wrong reason");
    }
    {
        auto module = valid;
        module.variant_layouts[0].cases[0].reference_map.pop_back();
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutShape,
                "variant field/bitmap length mismatch used the wrong reason");
    }
    {
        auto module = valid;
        module.variant_layouts[0].cases[0].reference_map[0] = false;
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutShape,
                "wrong derived variant bitmap used the wrong reason");
    }
    {
        auto module = valid;
        module.variant_layouts[0].cases[0].field_types[0] =
            lang::map_signature(lang::variant_signature(0),
                                lang::signature_value(lang::ValueKind::Int64));
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutShape,
                "variant map key used the wrong layout-shape reason");
    }
    {
        auto module = valid;
        module.functions[0].code[4].operand = 9;
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutIndex,
                "bad AllocVariant layout used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].code[4].operand2 = 9;
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantCaseIndex,
                "bad AllocVariant case used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].code[12].operand = 9;
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantLayoutIndex,
                "bad VariantGet layout used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].code[12].operand2 = 9;
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantCaseIndex,
                "bad VariantGet case used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].code[12].operand3 = 9;
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantFieldIndex,
                "bad VariantGet field used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].code.erase(module.functions[0].code.begin(),
                                       module.functions[0].code.begin() + 3);
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantInitializerArity,
                "missing variant initializer used the wrong reason");
    }
    {
        auto module = valid;
        module.variant_layouts[0].cases[0].field_types[1] =
            lang::signature_value(lang::ValueKind::Bool);
        require(first_reason(module) ==
                    lang::VerifierReason::BadVariantInitializerType,
                "wrong variant initializer type used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].local_count = 0;
        module.functions[0].code = {
            {lang::OpCode::ConstantI64, 1},
            {lang::OpCode::ConstantI64, 2},
            {lang::OpCode::AllocPair, 0},
            {lang::OpCode::VariantTag, 0},
            {lang::OpCode::Return, 0},
        };
        require(first_reason(module) ==
                    lang::VerifierReason::VariantOperationOnNonVariant,
                "VariantTag on pair used the wrong reason");
    }
    {
        auto module = valid;
        module.functions[0].local_count = 0;
        module.functions[0].code = {
            {lang::OpCode::ConstantI64, 1},
            {lang::OpCode::ConstantI64, 2},
            {lang::OpCode::AllocPair, 0},
            {lang::OpCode::VariantGet, 0, 0, 0},
            {lang::OpCode::Return, 0},
        };
        require(first_reason(module) ==
                    lang::VerifierReason::VariantOperationOnNonVariant,
                "VariantGet on pair used the wrong reason");
    }
    {
        auto module = valid;
        auto other = choice_layout();
        other.name = "OtherChoice";
        module.variant_layouts.push_back(std::move(other));
        module.functions[0].code[12].operand = 2;
        require(first_reason(module) ==
                    lang::VerifierReason::VariantLayoutMismatch,
                "cross-layout variant access used the wrong reason");
    }
    {
        lang::Module module;
        module.variant_layouts.push_back(choice_layout());
        lang::Function function;
        function.signature = make_function_signature(
            {lang::variant_signature(0)},
            lang::signature_value(lang::ValueKind::Int64));
        function.local_count = 1;
        function.code = {
            {lang::OpCode::LoadLocal, 0},
            {lang::OpCode::VariantTag, 0},
            {lang::OpCode::Return, 0},
        };
        module.functions.push_back(std::move(function));
        require(first_reason(module) ==
                    lang::VerifierReason::VariantReceiverMayBeNil,
                "possibly-nil VariantTag used the wrong reason");
    }
    {
        lang::Module module;
        module.variant_layouts.push_back(choice_layout());
        lang::Function function;
        function.signature = make_function_signature(
            {lang::variant_signature(0)},
            lang::signature_value(lang::ValueKind::Int64));
        function.local_count = 1;
        function.code = {
            {lang::OpCode::LoadLocal, 0},
            {lang::OpCode::VariantGet, 0, 0, 1},
            {lang::OpCode::Return, 0},
        };
        module.functions.push_back(std::move(function));
        require(first_reason(module) ==
                    lang::VerifierReason::VariantReceiverMayBeNil,
                "possibly-nil VariantGet used the wrong reason");
    }
    {
        lang::Module module;
        module.variant_layouts.push_back(choice_layout());
        lang::Function function;
        function.signature = make_function_signature(
            {lang::variant_signature(0)},
            lang::signature_value(lang::ValueKind::Int64));
        function.local_count = 1;
        function.code = {
            {lang::OpCode::LoadLocal, 0},
            {lang::OpCode::IsNil, 0},
            {lang::OpCode::JumpIfFalse, 5},
            {lang::OpCode::ConstantI64, 0},
            {lang::OpCode::Return, 0},
            {lang::OpCode::LoadLocal, 0},
            {lang::OpCode::VariantTag, 0},
            {lang::OpCode::Return, 0},
        };
        module.functions.push_back(std::move(function));
        require(lang::verify_module_with_diagnostics(std::move(module))
                    .module.has_value(),
                "IsNil false-edge did not refine a nullable variant receiver");
    }

    const std::vector<std::pair<lang::VerifierReason, std::string_view>> names = {
        {lang::VerifierReason::BadVariantLayoutShape,
         "BadVariantLayoutShape"},
        {lang::VerifierReason::BadVariantLayoutIndex,
         "BadVariantLayoutIndex"},
        {lang::VerifierReason::BadVariantCaseIndex,
         "BadVariantCaseIndex"},
        {lang::VerifierReason::BadVariantFieldIndex,
         "BadVariantFieldIndex"},
        {lang::VerifierReason::BadVariantInitializerArity,
         "BadVariantInitializerArity"},
        {lang::VerifierReason::BadVariantInitializerType,
         "BadVariantInitializerType"},
        {lang::VerifierReason::VariantOperationOnNonVariant,
         "VariantOperationOnNonVariant"},
        {lang::VerifierReason::VariantLayoutMismatch,
         "VariantLayoutMismatch"},
        {lang::VerifierReason::VariantReceiverMayBeNil,
         "VariantReceiverMayBeNil"},
    };
    for (const auto& [reason, expected] : names) {
        require(lang::verifier_reason_name(reason) == expected,
                std::string("unstable variant verifier reason name: ") +
                    std::string(expected));
    }
}

void tag_width_and_case_mismatch_traps() {
    lang::gc::Heap heap;
    require_throws(
        [&] { (void)heap.allocate_variant(0, 2, {}, {{}, {}}); },
        "variant case tag out of range",
        "out-of-range raw variant tag did not trap stably");
    require_throws(
        [&] {
            (void)heap.allocate_variant(
                0, 0, {lang::Value::int64(1)}, {{false, false}, {}});
        },
        "variant payload width does not match selected case",
        "active payload width disagreement did not trap stably");
    require_throws(
        [&] {
            (void)heap.allocate_variant(
                0, 0, {lang::Value::int64(1)}, {{true}, {}});
        },
        "reference",
        "variant bitmap/tagged-value disagreement did not trap");

    const auto wide = heap.allocate_variant(
        0, 0,
        {lang::Value::int64(1), lang::Value::int64(2),
         lang::Value::int64(3), lang::Value::int64(4)},
        {{false, false, false, false}, {}});
    const auto narrow = heap.allocate_variant(0, 1, {},
                                              {{false, false, false, false}, {}});
    const auto tail = heap.allocate_pair(lang::Value::int64(5),
                                         lang::Value::int64(6));
    VectorRoots roots;
    roots.roots = {lang::Value::object(narrow), lang::Value::object(tail)};
    heap.collect(roots);
    require(slot_of(roots.roots.at(0).as_object()) == 0 &&
                slot_of(roots.roots.at(1).as_object()) == 2,
            "variant compaction cursor did not use exact 2 + active width");
    (void)wide;

    auto report = lang::verify_module_with_diagnostics(
        sibling_case_mismatch_module());
    require(report.module.has_value(),
            "verifier rejected a statically valid sibling-case get");
    for (const auto& schedule : fuzz::schedules()) {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        require_throws([&] { (void)vm.execute(*report.module); },
                       "variant case tag mismatch",
                       std::string("wrong runtime case did not trap under ") +
                           schedule.name);
    }
}

void match_arm_binding_roots() {
    constexpr std::string_view source = R"(
variant Choice { Left(pair<i64, i64>, i64), Right(i64, pair<i64, i64>) }
fn pick(flag: bool) -> i64 {
  let value: Choice = Choice.Left(pair(1, 2), 10);
  let churn: pair<i64, i64> = pair(0, 0);
  let result: i64 = 0;
  if flag { } else { value = Choice.Right(20, pair(3, 4)); }
  match value {
    Right(n, p) => { churn = pair(90, 91); result = n + p.left; },
    Left(p, n) => { churn = pair(92, 93); result = p.left + n; }
  }
  result
}
pick(true) + pick(false)
)";
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::string message = "frontend rejected valid exhaustive variant match:";
        for (const auto& diagnostic : compiled.diagnostics) {
            message += " [" + std::to_string(diagnostic.position.line) + ":" +
                       std::to_string(diagnostic.position.column) + "] " +
                       diagnostic.message;
        }
        require(false, message);
    }
    require(compiled.result_type == lang::frontend::Type::Int64,
            "match program inferred the wrong result type");

    const auto& module = compiled.verified_module->module();
    const auto& proof = compiled.verified_module->verification();
    require(module.variant_layouts.size() == 1,
            "compiler did not emit one nominal variant layout");
    bool saw_ref_binding = false;
    bool saw_scalar_binding = false;
    for (std::size_t function_index = 0;
         function_index < module.functions.size(); ++function_index) {
        const auto& function = module.functions[function_index];
        for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
            const auto& instruction = function.code[pc];
            if (instruction.op != lang::OpCode::VariantGet) {
                continue;
            }
            require(pc + 1 < function.code.size() &&
                        function.code[pc + 1].op == lang::OpCode::StoreLocal,
                    "match binding get was not immediately stored");
            const auto layout = static_cast<std::size_t>(instruction.operand);
            const auto case_index = static_cast<std::size_t>(instruction.operand2);
            const auto field_index = static_cast<std::size_t>(instruction.operand3);
            const bool is_ref = module.variant_layouts.at(layout)
                                    .cases.at(case_index)
                                    .reference_map.at(field_index);
            const auto& get_map = proof.functions.at(function_index)
                                      .stack_maps.at(pc + 1);
            require(!get_map.object_slots.empty() &&
                        get_map.object_slots.back() == is_ref,
                    "VariantGet result root bit disagreed with selected field");
            const auto local = static_cast<std::size_t>(
                function.code[pc + 1].operand);
            const auto& after_store = proof.functions.at(function_index)
                                          .stack_maps.at(pc + 2);
            require(after_store.local_object_slots.at(local) == is_ref,
                    "match binding local root bit was not exact after store");
            saw_ref_binding = saw_ref_binding || is_ref;
            saw_scalar_binding = saw_scalar_binding || !is_ref;

            const bool is_last_binding =
                pc + 2 >= function.code.size() ||
                function.code[pc + 2].op != lang::OpCode::LoadLocal ||
                pc + 3 >= function.code.size() ||
                function.code[pc + 3].op != lang::OpCode::VariantGet;
            if (is_last_binding) {
                const auto case_fields = module.variant_layouts.at(layout)
                                             .cases.at(case_index)
                                             .reference_map.size();
                require(case_fields > 0 && pc + 2 >= 3 * case_fields,
                        "match arm binding sequence was malformed");
                const auto& arm_entry = proof.functions.at(function_index)
                                            .stack_maps.at(pc + 2);
                for (std::size_t sibling = 0; sibling < case_fields; ++sibling) {
                    const auto get_pc = pc - 3 * (case_fields - 1 - sibling);
                    require(function.code.at(get_pc).op ==
                                lang::OpCode::VariantGet &&
                                static_cast<std::size_t>(
                                    function.code.at(get_pc).operand2) ==
                                    case_index &&
                                function.code.at(get_pc + 1).op ==
                                    lang::OpCode::StoreLocal,
                            "match arm bindings were not emitted as one guarded group");
                    const auto sibling_local = static_cast<std::size_t>(
                        function.code.at(get_pc + 1).operand);
                    require(arm_entry.local_object_slots.at(sibling_local) ==
                                module.variant_layouts.at(layout)
                                    .cases.at(case_index)
                                    .reference_map.at(sibling),
                            "arm-entry root map did not keep all sibling bindings exact");
                }
            }
        }
    }
    require(saw_ref_binding && saw_scalar_binding,
            "match test did not inspect both reference and scalar bindings");

    lang::VM vm;
    vm.set_gc_stress(maximum_stress());
    require(vm.execute(*compiled.verified_module).as_i64() == 34,
            "match bindings did not survive compacting arm allocations");
    require(vm.metrics().heap.objects_moved > 0,
            "match binding root test did not exercise movement");
}

void nested_match_in_later_arm_joins_local_state() {
    constexpr std::string_view source = R"(
variant Inner { One(i64), Two(i64) }
variant Box { Empty(), Full(i64) }
let inner: Inner = Inner.One(7);
let box: Box = Box.Full(1);
let result: i64 = 0;
match box {
  Empty => { result = 0; },
  Full(ignored) => {
    match inner {
      One(value) => { result = value; },
      Two(value) => { result = value + 1; }
    }
  }
}
result
)";
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::string message = "frontend rejected a nested match in a later arm:";
        for (const auto& diagnostic : compiled.diagnostics) {
            message += " [" + std::to_string(diagnostic.position.line) + ":" +
                       std::to_string(diagnostic.position.column) + "] " +
                       diagnostic.message;
        }
        require(false, message);
    }
    lang::VM vm;
    require(vm.execute(*compiled.verified_module).as_i64() == 7,
            "nested match in a later arm changed the joined local state");
}

void recursive_binary_tree_all_schedules() {
    constexpr std::string_view source = R"(
variant Tree { Leaf(), Node(i64, Tree, Tree) }
fn sum(tree: Tree) -> i64 {
  let result: i64 = 0;
  if is_nil(tree) {
    result = 0;
  } else {
    match tree {
      Leaf => { result = 1; },
      Node(value, left, right) => { result = value + sum(left) + sum(right); }
    }
  }
  result
}
let leaf: Tree = Tree.Leaf();
let tree: Tree = Tree.Node(10, Tree.Node(20, leaf, Tree.Leaf()), Tree.Leaf());
let gap: pair<i64, i64> = pair(1, 2);
let survivor: pair<i64, i64> = pair(3, 4);
gap = pair(6, 7);
let movement_trigger: pair<i64, i64> = pair(survivor.left, 5);
let total: i64 = sum(tree);
print(to_str(total));
total
)";
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::string message = "frontend rejected recursive binary-tree variant:";
        for (const auto& diagnostic : compiled.diagnostics) {
            message += " [" + std::to_string(diagnostic.position.line) + ":" +
                       std::to_string(diagnostic.position.column) + "] " +
                       diagnostic.message;
        }
        require(false, message);
    }
    lang::VM oracle_vm;
    const auto oracle_result = oracle_vm.execute(*compiled.verified_module).as_i64();
    const auto oracle_output = oracle_vm.output();
    require(oracle_result == 33 &&
                std::string(oracle_output.begin(), oracle_output.end()) == "33\n",
            "recursive variant no-stress oracle changed");
    bool stressed_movement = false;
    for (const auto& schedule : fuzz::schedules()) {
        lang::VM vm;
        vm.set_gc_stress(schedule.stress);
        require(vm.execute(*compiled.verified_module).as_i64() == oracle_result,
                std::string("recursive variant changed result under ") +
                    schedule.name);
        require(vm.output() == oracle_output,
                std::string("recursive variant changed output under ") +
                    schedule.name);
        if (std::string_view(schedule.name) != "no_stress") {
            stressed_movement = stressed_movement ||
                                vm.metrics().heap.objects_moved > 0;
        }
        if (std::string_view(schedule.name) == "combined") {
            require(vm.metrics().heap.objects_moved > 0,
                    "combined recursive schedule did not move live variants");
        }
    }
    require(stressed_movement,
            "recursive variant schedules never exercised object movement");
}

void promotion_edge_non_vacuity() {
    lang::gc::Heap heap;
    const auto young = heap.allocate_pair(lang::Value::int64(41),
                                          lang::Value::int64(42));
    const auto owner = heap.allocate_variant(
        0, 0, {lang::Value::object(young)}, {{true}, {false}});
    const auto barriers_before = heap.metrics().write_barrier_hits;
    heap.TEST_ONLY_promote_object_through_collector_path(owner);
    require(heap.TEST_ONLY_remembered_set_size() == 1 &&
                heap.metrics().write_barrier_hits == barriers_before,
            "promotion-created variant edge was not recorded collector-internally");

    lang::gc::Heap scalar_heap;
    const auto scalar = scalar_heap.allocate_variant(
        0, 1, {lang::Value::int64(static_cast<std::int64_t>(young))},
        {{true}, {false}});
    scalar_heap.TEST_ONLY_promote_object_through_collector_path(scalar);
    require(scalar_heap.TEST_ONLY_remembered_set_size() == 0,
            "scalar-only promoted variant entered the remembered set");
}

void require_positioned_error(std::string_view source, std::size_t line,
                              std::string_view text) {
    const auto compiled = lang::frontend::compile_program(source);
    require(!compiled.ok() && !compiled.diagnostics.empty(),
            "invalid variant source produced no diagnostic");
    const auto& diagnostic = compiled.diagnostics.front();
    require(diagnostic.position.line == line,
            "variant diagnostic was reported at the wrong line");
    require(diagnostic.message.find(text) != std::string::npos,
            "variant diagnostic omitted the stable rejection detail");
}

void frontend_variant_declarations_and_construction() {
    constexpr std::string_view source = R"(
variant Choice { Empty(), Left(i64, pair<i64, i64>), Right(pair<i64, i64>, bool) }
let empty: Choice = Choice.Empty();
let left_value: Choice = Choice.Left(7, pair(8, 9));
let right_value: Choice = Choice.Right(pair(10, 11), true);
let values: [Choice] = [Choice.Empty()];
let holder: pair<Choice, Choice> = pair(nil, nil);
let repeat: bool = true;
while repeat {
    holder.left = Choice.Empty();
    values[0] = Choice.Empty();
    repeat = false;
}
0
)";
    const auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(),
            "frontend rejected valid variant declarations or constructors");
    require(compiled.result_type == lang::frontend::Type::Int64,
            "variant constructor program inferred the wrong result type");

    const auto& module = compiled.verified_module->module();
    require(module.variant_layouts.size() == 1,
            "compiler did not emit exactly one variant layout");
    const auto& layout = module.variant_layouts.front();
    require(layout.name == "Choice" && layout.cases.size() == 3,
            "variant layout name or source-order cases changed");
    require(layout.cases.at(0).name == "Empty" &&
                layout.cases.at(0).field_types.empty() &&
                layout.cases.at(0).reference_map.empty(),
            "zero-field variant case layout was not preserved");
    require(layout.cases.at(1).name == "Left" &&
                layout.cases.at(1).reference_map ==
                    std::vector<bool>({false, true}) &&
                layout.cases.at(2).name == "Right" &&
                layout.cases.at(2).reference_map ==
                    std::vector<bool>({true, false}),
            "variant case reference maps disagreed with payload types");

    std::vector<std::pair<std::int64_t, std::int64_t>> allocations;
    for (const auto& function : module.functions) {
        for (const auto& instruction : function.code) {
            if (instruction.op == lang::OpCode::AllocVariant) {
                allocations.emplace_back(instruction.operand,
                                         instruction.operand2);
            }
        }
    }
    require(allocations ==
                std::vector<std::pair<std::int64_t, std::int64_t>>(
                    {{0, 0}, {0, 1}, {0, 2}, {0, 0}, {0, 0}, {0, 0}}),
            "constructors did not lower to exact source-order layout/case operands");

    lang::VM vm;
    vm.set_gc_stress(maximum_stress());
    require(vm.execute(*compiled.verified_module).as_i64() == 0,
            "variant constructors did not execute under maximum GC stress");

    require_positioned_error(
        "variant Choice { Empty() }\n"
        "let value: Choice = Choice.Missing();\n"
        "0\n",
        2, "unknown variant case 'Missing'");
    require_positioned_error(
        "variant Choice { Pair(i64, bool) }\n"
        "let value: Choice = Choice.Pair(1);\n"
        "0\n",
        2, "expects 2 payload values");
    require_positioned_error(
        "variant Choice { Number(i64) }\n"
        "let value: Choice = Choice.Number(true);\n"
        "0\n",
        2, "variant payload 0");
    require_positioned_error(
        "variant A { One(i64) }\n"
        "variant B { One(i64) }\n"
        "let a: A = A.One(1);\n"
        "let b: B = B.One(2);\n"
        "a = b;\n"
        "0\n",
        5, "cannot assign B to local 'a' of type A");

    const auto fixed_point_error = lang::frontend::compile_program(
        "variant Choice { Pair(i64, bool) }\n"
        "let holder: pair<Choice, Choice> = pair(nil, nil);\n"
        "let repeat: bool = true;\n"
        "while repeat {\n"
        "    holder.left = Choice.Pair(1, true);\n"
        "    holder.left = Choice.Pair(true);\n"
        "    repeat = false;\n"
        "}\n"
        "0\n");
    require(!fixed_point_error.ok(),
            "malformed constructor in fixed-point loop was accepted");
    std::size_t arity_diagnostics = 0;
    std::size_t payload_diagnostics = 0;
    for (const auto& diagnostic : fixed_point_error.diagnostics) {
        require(diagnostic.position.line == 6,
                "fixed-point constructor diagnostic moved off its source line");
        arity_diagnostics +=
            diagnostic.message.find("expects 2 payload values") !=
            std::string::npos;
        payload_diagnostics +=
            diagnostic.message.find("variant payload 0") != std::string::npos;
    }
    require(fixed_point_error.diagnostics.size() == 2 &&
                arity_diagnostics == 1 && payload_diagnostics == 1,
            "fixed-point constructor diagnostics were not stable and idempotent");
}

void positioned_exhaustiveness_and_nominal_rejections() {
    require_positioned_error(
        "variant V { A(i64), B(i64) }\n"
        "let v: V = V.A(1);\n"
        "match v { A(x) => { print(to_str(x)); } }\n"
        "0\n",
        3, "B");
    require_positioned_error(
        "variant A { One(i64) }\n"
        "variant B { One(i64) }\n"
        "let a: A = A.One(1);\n"
        "let b: B = B.One(2);\n"
        "a = b;\n"
        "0\n",
        5, "cannot assign B to local 'a' of type A");
    require_positioned_error(
        "variant V { One(i64) }\n"
        "let v: V = V.One(1);\n"
        "match v { One(x) => { x = 2; } }\n"
        "0\n",
        3, "cannot assign to immutable match binding 'x'");
    require_positioned_error(
        "variant V { A(i64) }\nlet v: V = V.A(1);\n"
        "match v { Missing(x) => { print(to_str(x)); } }\n0\n",
        3, "unknown variant case 'Missing'");
    require_positioned_error(
        "variant V { A(i64) }\nlet v: V = V.A(1);\n"
        "match v { A(x) => {}, A(y) => {} }\n0\n",
        3, "duplicate match arm for case 'A'");
    require_positioned_error(
        "variant V { A(i64, i64) }\nlet v: V = V.A(1, 2);\n"
        "match v { A(x, x) => {} }\n0\n",
        3, "duplicate match binding 'x'");
    require_positioned_error(
        "variant V { A(i64, i64) }\nlet v: V = V.A(1, 2);\n"
        "match v { A(x) => {} }\n0\n",
        3, "expects 2 bindings but got 1");
    require_positioned_error(
        "variant V { A(i64) }\nlet v: V = V.A(1);\nlet x: i64 = 0;\n"
        "match v { A(x) => {} }\n0\n",
        4, "conflicts with an existing local");
    require_positioned_error(
        "variant V { A(i64) }\nfn outer(x: i64) -> i64 {\n"
        "  let f: fn() -> i64 = fn() -> i64 { let v: V = V.A(1); "
        "match v { A(x) => {} } x };\n  f()\n}\nouter(1)\n",
        3, "conflicts with an existing capture");
    const auto repeated_capture_conflict = lang::frontend::compile_program(R"(
variant V { A(i64) }
fn outer(x: i64) -> i64 {
  let f: fn() -> i64 = fn() -> i64 {
    let repeat: bool = true;
    while repeat {
      match V.A(1) { A(x) => {} }
      repeat = false;
    }
    match V.A(2) { A(x) => {} }
    0
  };
  f()
}
outer(7)
)");
    require(!repeated_capture_conflict.ok() &&
                repeated_capture_conflict.diagnostics.size() == 2,
            "fixed-point capture conflicts did not produce an exact stable set");
    require(repeated_capture_conflict.diagnostics.at(0).position.line == 7 &&
                repeated_capture_conflict.diagnostics.at(1).position.line == 10 &&
                repeated_capture_conflict.diagnostics.at(0).message ==
                    "match binding 'x' conflicts with an existing capture" &&
                repeated_capture_conflict.diagnostics.at(1).message ==
                    "match binding 'x' conflicts with an existing capture",
            "nested capture-conflict diagnostics were duplicated or repositioned");
    require_positioned_error(
        "variant V { A() }\nlet v: V = nil;\nmatch v { A => {} }\n0\n",
        3, "requires a proven non-nil variant");
    require_positioned_error(
        "variant V { First(), Second(), Third(), Fourth() }\n"
        "let v: V = V.First();\nmatch v { First => {}, Third => {} }\n0\n",
        3, "Second, Fourth");

    const auto flow_program = lang::frontend::compile_program(R"(
variant V { A(i64), B(i64) }
fn nested() -> i64 {
  let result: i64 = 0;
  match V.A(9) {
    A(x) => { match V.B(x) { A(y) => { result = y; }, B(y) => { result = y; } } },
    B(x) => { result = x; }
  }
  result
}
let loop: bool = true;
while loop {
  match V.A(7) { A(x) => { loop = false; continue; }, B(y) => { break; } }
}
nested()
)");
    std::string flow_error =
        "nested match or break/continue binding deactivation was rejected";
    for (const auto& diagnostic : flow_program.diagnostics) {
        flow_error += " [" + std::to_string(diagnostic.position.line) + ":" +
                      std::to_string(diagnostic.position.column) + "] " +
                      diagnostic.message;
    }
    require(flow_program.ok(), flow_error);

    const auto terminating = lang::frontend::compile_program(R"(
variant V { A(), B() }
fn choose() -> i64 {
  let v: V = V.A();
  while true {
    match v { A => { break; }, B => { break; } }
  }
  1
}
choose()
)");
    std::string terminating_error =
        "match with all terminating arms corrupted flow analysis";
    for (const auto& diagnostic : terminating.diagnostics) {
        terminating_error += " [" +
                             std::to_string(diagnostic.position.line) + ":" +
                             std::to_string(diagnostic.position.column) + "] " +
                             diagnostic.message;
    }
    require(terminating.ok(), terminating_error);

    const auto fixed_point = lang::frontend::compile_program(R"(
variant V { A(i64), B(i64) }
let again: bool = true;
while again {
  match V.A(1) { A(x) => { print(to_str(x)); }, B(y) => { print(to_str(y)); } }
  again = false;
}
0
)");
    require(fixed_point.ok(),
            "fixed-point match-local allocation was not idempotent");

    require(std::string(lang::verifier_reason_name(
                lang::VerifierReason::BadVariantLayoutShape)) ==
                "BadVariantLayoutShape" &&
                std::string(lang::verifier_reason_name(
                    lang::VerifierReason::VariantReceiverMayBeNil)) ==
                    "VariantReceiverMayBeNil",
            "variant verifier reason names are not stable");
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main(int argc, char** argv) {
    const std::vector<TestCase> tests = {
        {"per_case_bitmap_precision", per_case_bitmap_precision},
        {"backend_verifier_accepts_variants_and_rejects_malformed_modules",
         backend_verifier_accepts_variants_and_rejects_malformed_modules},
        {"tag_width_and_case_mismatch_traps",
         tag_width_and_case_mismatch_traps},
        {"match_arm_binding_roots", match_arm_binding_roots},
        {"nested_match_in_later_arm_joins_local_state",
         nested_match_in_later_arm_joins_local_state},
        {"recursive_binary_tree_all_schedules",
         recursive_binary_tree_all_schedules},
        {"promotion_edge_non_vacuity", promotion_edge_non_vacuity},
        {"frontend_variant_declarations_and_construction",
         frontend_variant_declarations_and_construction},
        {"positioned_exhaustiveness_and_nominal_rejections",
         positioned_exhaustiveness_and_nominal_rejections},
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
        std::cerr << "unknown iteration-34 variant test: " << selected << "\n";
        return 2;
    }
    if (failures != 0) {
        std::cerr << failures << " iteration-34 variant test(s) failed\n";
        return 1;
    }
    return 0;
}
