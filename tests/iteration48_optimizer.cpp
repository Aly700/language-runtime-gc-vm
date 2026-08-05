#include "lang/frontend/type_checker.hpp"
#include "lang/optimizer.hpp"
#include "lang/vm.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
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

lang::Module constant_module(std::int64_t value) {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);
    auto& function = module.functions.front();
    function.signature.return_type = lang::ValueKind::Int64;
    function.code = {
        {lang::OpCode::ConstantI64, value},
        {lang::OpCode::Return, 0},
    };
    return module;
}

lang::Module binary_module(lang::OpCode op, std::int64_t left,
                           std::int64_t right,
                           lang::ValueKind result_kind) {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);
    auto& function = module.functions.front();
    function.signature.return_type = result_kind;
    function.code = {
        {lang::OpCode::ConstantI64, left},
        {lang::OpCode::ConstantI64, right},
        {op, 0},
        {lang::OpCode::Return, 0},
    };
    function.source_positions = {
        {1, 2},
        {1, 6},
        {1, 4},
        {1, 8},
    };
    return module;
}

lang::OptimizerOptions folding_only() {
    return lang::OptimizerOptions{
        .constant_folding = true,
        .dead_code_elimination = false,
        .peephole = false,
    };
}

lang::Module constant_object_branch_module(bool condition) {
    lang::Module module;
    module.functions.resize(1);
    auto& function = module.functions.front();
    function.signature.return_type = lang::ValueKind::Object;
    function.code = {
        {lang::OpCode::ConstantI64, condition ? 0 : 2},
        {lang::OpCode::ConstantI64, condition ? 1 : 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::JumpIfFalse, 8},
        {lang::OpCode::ConstantI64, 10},
        {lang::OpCode::ConstantI64, 11},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Jump, 11},
        {lang::OpCode::ConstantI64, 20},
        {lang::OpCode::ConstantI64, 21},
        {lang::OpCode::AllocPair, 0},
        {lang::OpCode::Return, 0},
    };
    return module;
}

void require_maps_equal(const std::vector<lang::StackMap>& left,
                        const std::vector<lang::StackMap>& right,
                        const std::string& context) {
    require(left.size() == right.size(), context + ": map count differs");
    for (std::size_t pc = 0; pc < left.size(); ++pc) {
        require(left[pc].object_slots == right[pc].object_slots,
                context + ": operand object bits differ at pc " +
                    std::to_string(pc));
        require(left[pc].local_object_slots ==
                    right[pc].local_object_slots,
                context + ": local object bits differ at pc " +
                    std::to_string(pc));
    }
}

void all_passes_off_reverify_without_effects() {
    const lang::OptimizerOptions options{
        .constant_folding = false,
        .dead_code_elimination = false,
        .peephole = false,
    };
    const auto result = lang::optimize_module(constant_module(42), options);
    require(result.ok(), "all-false optimizer rejected valid input");
    require(result.verified_module.has_value(),
            "all-false optimizer omitted verifier proof");
    require(result.diagnostics.empty(),
            "all-false optimizer reported verifier diagnostics");
    require(result.stats.instructions_before == 2 &&
                result.stats.instructions_after == 2,
            "all-false optimizer changed instruction count");
    require(result.stats.folds_applied == 0 &&
                result.stats.blocks_eliminated == 0 &&
                result.stats.peepholes_applied == 0,
            "all-false optimizer reported effects");

    const auto& function =
        result.verified_module->module().functions.front();
    require(function.code.size() == 2 &&
                function.code[0].op == lang::OpCode::ConstantI64 &&
                function.code[0].operand == 42 &&
                function.code[1].op == lang::OpCode::Return,
            "all-false optimizer changed bytecode");
    require(function.stack_maps.size() == function.code.size(),
            "all-false optimizer did not attach regenerated stack maps");

    lang::VM vm;
    require(vm.execute(*result.verified_module).as_i64() == 42,
            "all-false optimized module returned the wrong value");
}

void invalid_input_is_rejected_before_optimization() {
    auto module = constant_module(0);
    module.functions.front().code.front() = {lang::OpCode::AddI64, 0};

    const auto result = lang::optimize_module(std::move(module));
    require(!result.ok() && !result.verified_module.has_value(),
            "optimizer accepted invalid raw input");
    require(!result.diagnostics.empty() &&
                result.diagnostics.front().reason ==
                    lang::VerifierReason::StackUnderflow,
            "optimizer lost the input verifier rejection");
}

void deliberately_invalid_output_is_caught_by_reverification() {
    const auto result =
        lang::TEST_ONLY_optimize_module_with_invalid_output(
            constant_module(7));
    require(!result.ok() && !result.verified_module.has_value(),
            "broken optimizer output acquired a verifier proof");
    require(!result.diagnostics.empty() &&
                result.diagnostics.front().reason ==
                    lang::VerifierReason::StackUnderflow,
            "broken optimizer output was not caught by final verification");
}

void constant_folding_matrix_preserves_exact_semantics() {
    {
        auto result = lang::optimize_module(
            binary_module(lang::OpCode::AddI64, 40, 2,
                          lang::ValueKind::Int64),
            folding_only());
        require(result.ok() && result.stats.folds_applied == 1,
                "constant i64 addition was not folded");
        const auto& function =
            result.verified_module->module().functions.front();
        require(function.code.size() == 2 &&
                    function.code[0].op == lang::OpCode::ConstantI64 &&
                    function.code[0].operand == 42,
                "constant i64 addition folded to the wrong bytecode");
        require(function.source_positions.size() == function.code.size() &&
                    function.source_positions[0] ==
                        lang::DebugSourcePosition{1, 4},
                "folded addition did not retain the operator source position");
        lang::VM vm;
        require(vm.execute(*result.verified_module).as_i64() == 42,
                "folded addition returned the wrong value");
    }

    {
        auto result = lang::optimize_module(
            binary_module(
                lang::OpCode::AddI64,
                std::numeric_limits<std::int64_t>::max(), 1,
                lang::ValueKind::Int64),
            folding_only());
        require(result.ok() && result.stats.folds_applied == 1,
                "wrapping constant addition was not folded");
        lang::VM vm;
        require(
            vm.execute(*result.verified_module).as_i64() ==
                std::numeric_limits<std::int64_t>::min(),
            "folded addition did not use modulo-2^64 wrapping");
    }

    {
        auto result = lang::optimize_module(
            binary_module(lang::OpCode::LessI64, -3, 9,
                          lang::ValueKind::Bool),
            folding_only());
        require(result.ok() && result.stats.folds_applied == 1,
                "constant i64 comparison was not folded");
        const auto& code =
            result.verified_module->module().functions.front().code;
        require(code.size() == 2 &&
                    code[0].op == lang::OpCode::ConstantBool &&
                    code[0].operand == 1,
                "constant comparison did not become true ConstantBool");
        lang::VM vm;
        require(vm.execute(*result.verified_module).as_bool(),
                "folded comparison returned false");
    }

    {
        auto module = constant_module(-7);
        module.functions.front().code.insert(
            module.functions.front().code.begin() + 1,
            {lang::OpCode::I64Abs, 0});
        auto result =
            lang::optimize_module(std::move(module), folding_only());
        require(result.ok() && result.stats.folds_applied == 1,
                "safe constant absolute value was not folded");
        const auto& code =
            result.verified_module->module().functions.front().code;
        require(code.size() == 2 && code[0].operand == 7,
                "safe absolute value folded to the wrong value");
    }

    {
        lang::Module module;
        module.functions.resize(1);
        auto& function = module.functions.front();
        function.signature.return_type = lang::ValueKind::Bool;
        function.code = {
            {lang::OpCode::Nil, 0},
            {lang::OpCode::IsNil, 0},
            {lang::OpCode::Return, 0},
        };
        auto result =
            lang::optimize_module(std::move(module), folding_only());
        require(result.ok() && result.stats.folds_applied == 1,
                "nil is_nil was not folded");
        const auto& code =
            result.verified_module->module().functions.front().code;
        require(code.size() == 2 &&
                    code[0].op == lang::OpCode::ConstantBool &&
                    code[0].operand == 1,
                "nil is_nil did not fold to true");
    }
}

void trapping_absolute_value_remains_runtime_bytecode() {
    auto module = constant_module(std::numeric_limits<std::int64_t>::min());
    module.functions.front().code.insert(
        module.functions.front().code.begin() + 1,
        {lang::OpCode::I64Abs, 0});

    auto result = lang::optimize_module(std::move(module), folding_only());
    require(result.ok() && result.stats.folds_applied == 0,
            "abs(INT64_MIN) was incorrectly folded");
    const auto& code =
        result.verified_module->module().functions.front().code;
    require(code.size() == 3 &&
                code[1].op == lang::OpCode::I64Abs,
            "abs(INT64_MIN) runtime trap instruction was removed");

    lang::VM vm;
    try {
        (void)vm.execute(*result.verified_module);
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(
                    "absolute value overflow") != std::string::npos,
                "abs(INT64_MIN) changed trap kind");
        require(vm.last_trap_trace().has_value() &&
                    vm.last_trap_trace()->kind ==
                        lang::RuntimeFailureKind::Trap,
                "abs(INT64_MIN) did not retain RuntimeFailureKind::Trap");
        return;
    }
    throw std::runtime_error("abs(INT64_MIN) returned a value");
}

void invalid_bool_constant_is_rejected() {
    lang::Module module;
    module.functions.resize(1);
    auto& function = module.functions.front();
    function.signature.return_type = lang::ValueKind::Bool;
    function.code = {
        {lang::OpCode::ConstantBool, 2},
        {lang::OpCode::Return, 0},
    };

    const auto result = lang::optimize_module(std::move(module));
    require(!result.ok() && !result.diagnostics.empty(),
            "invalid ConstantBool operand was accepted");
    require(result.diagnostics.front().reason ==
                lang::VerifierReason::InvalidBoolConstant,
            "invalid ConstantBool used the wrong verifier reason");
}

void dce_removes_only_fold_created_unreachable_block_and_regenerates_maps() {
    const lang::OptimizerOptions options{
        .constant_folding = true,
        .dead_code_elimination = true,
        .peephole = false,
    };
    auto result =
        lang::optimize_module(constant_object_branch_module(false), options);
    require(result.ok(), "constant-false DCE rejected a valid module");
    require(result.stats.folds_applied == 1,
            "constant-false DCE did not consume the folded comparison");
    require(result.stats.blocks_eliminated == 1,
            "constant-false DCE reported the wrong eliminated block count");
    require(result.stats.instructions_before == 12 &&
                result.stats.instructions_after == 5,
            "constant-false DCE reported the wrong instruction effect");

    const auto& function =
        result.verified_module->module().functions.front();
    require(function.code.size() == 5 &&
                function.code[0].op == lang::OpCode::Jump &&
                function.code[0].operand == 1 &&
                function.code[1].op == lang::OpCode::ConstantI64 &&
                function.code[1].operand == 20 &&
                function.code[3].op == lang::OpCode::AllocPair &&
                function.code[4].op == lang::OpCode::Return,
            "constant-false DCE retained or misretargeted the dead arm");

    const auto fresh =
        lang::verify_with_diagnostics(result.verified_module->module());
    require(fresh.result.has_value(),
            "fresh verifier rejected optimized DCE output");
    require_maps_equal(
        function.stack_maps, fresh.result->functions.front().stack_maps,
        "constant-false DCE");
    require(function.stack_maps[0].object_slots.empty() &&
                function.stack_maps[1].object_slots.empty() &&
                function.stack_maps[2].object_slots ==
                    std::vector<bool>{false} &&
                function.stack_maps[3].object_slots ==
                    std::vector<bool>({false, false}) &&
                function.stack_maps[4].object_slots ==
                    std::vector<bool>{true},
            "constant-false DCE regenerated incorrect exact object maps");

    lang::VM vm;
    const auto value = vm.execute(*result.verified_module);
    require(vm.heap().left(value.as_object()).as_i64() == 20 &&
                vm.heap().right(value.as_object()).as_i64() == 21,
            "constant-false DCE selected the wrong branch");
}

void dce_toggle_and_constant_true_path_are_independent() {
    const lang::OptimizerOptions folding_without_dce{
        .constant_folding = true,
        .dead_code_elimination = false,
        .peephole = false,
    };
    const auto retained = lang::optimize_module(
        constant_object_branch_module(false), folding_without_dce);
    require(retained.ok() && retained.stats.folds_applied == 1 &&
                retained.stats.blocks_eliminated == 0 &&
                retained.stats.instructions_after == 10,
            "disabling DCE did not retain both verifier-reachable arms");

    const lang::OptimizerOptions dce_only{
        .constant_folding = false,
        .dead_code_elimination = true,
        .peephole = false,
    };
    auto hand_built = constant_object_branch_module(true);
    auto& code = hand_built.functions.front().code;
    code.erase(code.begin(), code.begin() + 3);
    code.insert(code.begin(), {lang::OpCode::ConstantBool, 1});
    code[1].operand = 6;
    code[5].operand = 9;
    const auto direct = lang::optimize_module(
        std::move(hand_built), dce_only);
    require(direct.ok() && direct.stats.folds_applied == 0 &&
                direct.stats.blocks_eliminated == 1,
            "DCE-only did not simplify a hand-built ConstantBool branch");
    lang::VM vm;
    const auto value = vm.execute(*direct.verified_module);
    require(vm.heap().left(value.as_object()).as_i64() == 10 &&
                vm.heap().right(value.as_object()).as_i64() == 11,
            "constant-true DCE selected the wrong branch");
}

void folding_never_crosses_an_external_control_flow_entry() {
    lang::Module module;
    module.functions.resize(1);
    auto& function = module.functions.front();
    function.signature.parameters = {lang::ValueKind::Bool};
    function.signature.return_type = lang::ValueKind::Int64;
    function.local_count = 1;
    function.code = {
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::JumpIfFalse, 4},
        {lang::OpCode::ConstantI64, 10},
        {lang::OpCode::Jump, 5},
        {lang::OpCode::ConstantI64, 20},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };

    const auto result =
        lang::optimize_module(std::move(module), folding_only());
    require(result.ok() && result.stats.folds_applied == 0,
            "folder crossed an externally entered instruction window");
    require(result.stats.instructions_before ==
                result.stats.instructions_after,
            "externally entered fold window changed instruction count");
}

lang::Module peephole_identity_module() {
    lang::Module module;
    module.functions.resize(1);
    auto& function = module.functions.front();
    function.signature.return_type = lang::ValueKind::Int64;
    function.local_count = 1;
    function.code = {
        {lang::OpCode::ConstantI64, 42},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Jump, 6},
        {lang::OpCode::Return, 0},
    };
    return module;
}

void peepholes_apply_only_the_documented_identity_rules() {
    const lang::OptimizerOptions options{
        .constant_folding = false,
        .dead_code_elimination = false,
        .peephole = true,
    };
    auto result =
        lang::optimize_module(peephole_identity_module(), options);
    require(result.ok(), "peephole optimizer rejected identity fixture");
    require(result.stats.peepholes_applied == 2,
            "identity fixture did not apply exactly add-zero and next-jump");
    require(result.stats.instructions_before == 7 &&
                result.stats.instructions_after == 4,
            "identity peepholes reported the wrong instruction effect");
    const auto& code =
        result.verified_module->module().functions.front().code;
    require(code.size() == 4 &&
                code[0].op == lang::OpCode::ConstantI64 &&
                code[1].op == lang::OpCode::StoreLocal &&
                code[2].op == lang::OpCode::LoadLocal &&
                code[3].op == lang::OpCode::Return,
            "identity peepholes removed or retained the wrong instructions");
    lang::VM vm;
    require(vm.execute(*result.verified_module).as_i64() == 42,
            "identity peepholes changed the returned value");

    const lang::OptimizerOptions disabled{
        .constant_folding = false,
        .dead_code_elimination = false,
        .peephole = false,
    };
    const auto retained =
        lang::optimize_module(peephole_identity_module(), disabled);
    require(retained.ok() && retained.stats.peepholes_applied == 0 &&
                retained.stats.instructions_after == 7,
            "disabling peepholes changed the identity fixture");
}

void jump_threading_retargets_chains_but_preserves_cycles() {
    lang::Module module;
    module.functions.resize(1);
    auto& function = module.functions.front();
    function.signature.return_type = lang::ValueKind::Int64;
    function.local_count = 1;
    function.code = {
        {lang::OpCode::ConstantI64, 0},
        {lang::OpCode::ConstantI64, 1},
        {lang::OpCode::LessI64, 0},
        {lang::OpCode::StoreLocal, 0},
        {lang::OpCode::LoadLocal, 0},
        {lang::OpCode::JumpIfFalse, 8},
        {lang::OpCode::ConstantI64, 10},
        {lang::OpCode::Jump, 10},
        {lang::OpCode::ConstantI64, 20},
        {lang::OpCode::Jump, 7},
        {lang::OpCode::Return, 0},
    };
    const lang::OptimizerOptions options{
        .constant_folding = false,
        .dead_code_elimination = false,
        .peephole = true,
    };
    auto threaded = lang::optimize_module(std::move(module), options);
    require(threaded.ok() &&
                threaded.stats.peepholes_applied >= 2,
            "jump chain was not threaded and simplified");
    const auto& code =
        threaded.verified_module->module().functions.front().code;
    for (const auto& instruction : code) {
        if (instruction.op != lang::OpCode::Jump &&
            instruction.op != lang::OpCode::JumpIfFalse) {
            continue;
        }
        const auto target =
            static_cast<std::size_t>(instruction.operand);
        require(code[target].op != lang::OpCode::Jump,
                "surviving branch still targets an unconditional jump");
    }
    lang::VM vm;
    require(vm.execute(*threaded.verified_module).as_i64() == 10,
            "jump threading changed selected branch value");

    lang::Module cycle;
    cycle.functions.resize(1);
    cycle.functions.front().signature.return_type =
        lang::ValueKind::Int64;
    cycle.functions.front().code = {
        {lang::OpCode::Jump, 1},
        {lang::OpCode::Jump, 0},
    };
    auto preserved = lang::optimize_module(std::move(cycle), options);
    require(preserved.ok() &&
                preserved.stats.peepholes_applied == 0,
            "jump cycle was rewritten or miscounted");
    const auto& cycle_code =
        preserved.verified_module->module().functions.front().code;
    require(cycle_code.size() == 2 && cycle_code[0].operand == 1 &&
                cycle_code[1].operand == 0,
            "jump cycle was not preserved byte-for-byte");
}

void require_default_modules_equal(const lang::Module& left,
                                   const lang::Module& right) {
    require(left.entry_function == right.entry_function,
            "default compile entry function drifted");
    require(left.functions.size() == right.functions.size(),
            "default compile function count drifted");
    require(left.string_constants == right.string_constants,
            "default compile string pool drifted");
    for (std::size_t function_index = 0;
         function_index < left.functions.size(); ++function_index) {
        const auto& left_function = left.functions[function_index];
        const auto& right_function = right.functions[function_index];
        require(left_function.signature.parameters ==
                    right_function.signature.parameters &&
                    left_function.signature.return_type ==
                    right_function.signature.return_type &&
                    left_function.local_count ==
                    right_function.local_count &&
                    left_function.closure_layout ==
                    right_function.closure_layout &&
                    left_function.debug_name ==
                    right_function.debug_name &&
                    left_function.source_positions ==
                    right_function.source_positions,
                "default compile function metadata drifted");
        require(left_function.code.size() ==
                    right_function.code.size(),
                "default compile code size drifted");
        for (std::size_t pc = 0; pc < left_function.code.size(); ++pc) {
            const auto& left_instruction = left_function.code[pc];
            const auto& right_instruction = right_function.code[pc];
            require(
                left_instruction.op == right_instruction.op &&
                    left_instruction.operand ==
                        right_instruction.operand &&
                    left_instruction.operand2 ==
                        right_instruction.operand2 &&
                    left_instruction.operand3 ==
                        right_instruction.operand3,
                "default compile instruction drifted at pc " +
                    std::to_string(pc));
        }
        require_maps_equal(left_function.stack_maps,
                           right_function.stack_maps,
                           "default compile");
    }
}

void compile_pipeline_is_opt_in_and_default_off() {
    const std::string source = R"(
let value: i64 = 40;
if true {
  value = value + 2;
} else {
  value = value + 100;
}
value + 0
)";

    const auto baseline = lang::frontend::compile_program(source);
    const lang::frontend::CompileOptions defaults;
    const auto explicit_default =
        lang::frontend::compile_program(source, defaults);
    require(baseline.ok() && explicit_default.ok(),
            "default optimizer fixture failed source compilation");
    require(!baseline.optimization_stats.has_value() &&
                !explicit_default.optimization_stats.has_value(),
            "default compilation reported optimizer effects");
    require_default_modules_equal(
        baseline.verified_module->module(),
        explicit_default.verified_module->module());

    lang::frontend::CompileOptions enabled;
    enabled.optimize = true;
    const auto optimized =
        lang::frontend::compile_program(source, enabled);
    require(optimized.ok() &&
                optimized.optimization_stats.has_value(),
            "opt-in source compilation did not return optimizer stats");
    const auto stats = *optimized.optimization_stats;
    require(stats.instructions_before >
                stats.instructions_after &&
                stats.folds_applied >= 1 &&
                stats.blocks_eliminated >= 1 &&
                stats.peepholes_applied >= 1,
            "opt-in source compilation did not exercise all three passes");

    const auto fresh =
        lang::verify_with_diagnostics(
            optimized.verified_module->module());
    require(fresh.result.has_value(),
            "optimized source module failed fresh re-verification");
    for (std::size_t function_index = 0;
         function_index <
         optimized.verified_module->module().functions.size();
         ++function_index) {
        require_maps_equal(
            optimized.verified_module->module()
                .functions[function_index]
                .stack_maps,
            fresh.result->functions[function_index].stack_maps,
            "optimized source compile");
    }

    lang::VM baseline_vm;
    lang::VM explicit_vm;
    lang::VM optimized_vm;
    require(baseline_vm.execute(*baseline.verified_module).as_i64() ==
                    42 &&
                explicit_vm.execute(
                    *explicit_default.verified_module).as_i64() ==
                    42 &&
                optimized_vm.execute(
                    *optimized.verified_module).as_i64() ==
                    42,
            "opt-in source compilation changed observable value");

    const auto bool_program =
        lang::frontend::compile_program("true\n", enabled);
    require(bool_program.ok(),
            "optimized bool source failed compilation");
    const auto& bool_code =
        bool_program.verified_module->module().functions.front().code;
    require(bool_code.size() == 2 &&
                bool_code.front().op == lang::OpCode::ConstantBool,
            "opt-in compiler did not expose folded ConstantBool bytecode");
}

struct TestCase {
    const char* name;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"all_passes_off_reverify_without_effects",
         all_passes_off_reverify_without_effects},
        {"invalid_input_is_rejected_before_optimization",
         invalid_input_is_rejected_before_optimization},
        {"deliberately_invalid_output_is_caught_by_reverification",
         deliberately_invalid_output_is_caught_by_reverification},
        {"constant_folding_matrix_preserves_exact_semantics",
         constant_folding_matrix_preserves_exact_semantics},
        {"trapping_absolute_value_remains_runtime_bytecode",
         trapping_absolute_value_remains_runtime_bytecode},
        {"invalid_bool_constant_is_rejected",
         invalid_bool_constant_is_rejected},
        {"dce_removes_only_fold_created_unreachable_block_and_regenerates_maps",
         dce_removes_only_fold_created_unreachable_block_and_regenerates_maps},
        {"dce_toggle_and_constant_true_path_are_independent",
         dce_toggle_and_constant_true_path_are_independent},
        {"folding_never_crosses_an_external_control_flow_entry",
         folding_never_crosses_an_external_control_flow_entry},
        {"peepholes_apply_only_the_documented_identity_rules",
         peepholes_apply_only_the_documented_identity_rules},
        {"jump_threading_retargets_chains_but_preserves_cycles",
         jump_threading_retargets_chains_but_preserves_cycles},
        {"compile_pipeline_is_opt_in_and_default_off",
         compile_pipeline_is_opt_in_and_default_off},
    };

    try {
        for (const auto& test : tests) {
            test.run();
        }
        std::cerr << "[PASS] lang_iteration48_optimizer tests="
                  << tests.size() << "\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] iteration48 optimizer: "
                  << error.what() << "\n";
        return 1;
    }
}
