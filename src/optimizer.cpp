#include "lang/optimizer.hpp"

#include "numeric.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace lang {

namespace {

std::uint64_t instruction_count(const Module& module) {
    std::uint64_t count = 0;
    for (const auto& function : module.functions) {
        count += static_cast<std::uint64_t>(function.code.size());
    }
    return count;
}

struct RewrittenInstruction {
    Instruction instruction;
    std::size_t source_pc{0};
};

using Rewrite = std::vector<std::optional<RewrittenInstruction>>;

Rewrite identity_rewrite(const Function& function) {
    Rewrite rewrite;
    rewrite.reserve(function.code.size());
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        rewrite.push_back(RewrittenInstruction{function.code[pc], pc});
    }
    return rewrite;
}

bool is_branch(OpCode op) {
    return op == OpCode::Jump || op == OpCode::JumpIfFalse;
}

bool is_terminal(OpCode op) {
    return op == OpCode::Return || op == OpCode::TailCall ||
           op == OpCode::Throw;
}

std::vector<bool> protected_entries(const Function& function) {
    std::vector<bool> protected_pc(function.code.size(), false);
    protected_pc.front() = true;
    for (const auto& instruction : function.code) {
        if (is_branch(instruction.op)) {
            assert(instruction.operand >= 0);
            const auto target =
                static_cast<std::size_t>(instruction.operand);
            assert(target < function.code.size());
            protected_pc[target] = true;
        }
    }
    for (const auto& handler : function.exception_handlers) {
        protected_pc[handler.try_begin] = true;
        protected_pc[handler.try_end] = true;
        protected_pc[handler.target] = true;
    }
    return protected_pc;
}

void apply_rewrite(Function& function, const Rewrite& rewrite) {
    assert(rewrite.size() == function.code.size());
    const auto old_size = function.code.size();
    const bool complete_debug_table =
        function.source_positions.size() == old_size;

    std::vector<std::optional<std::size_t>> old_to_new(old_size);
    std::size_t new_size = 0;
    for (std::size_t pc = 0; pc < old_size; ++pc) {
        if (rewrite[pc].has_value()) {
            old_to_new[pc] = new_size++;
        }
    }

    std::optional<std::size_t> next_survivor;
    for (std::size_t pc = old_size; pc > 0; --pc) {
        const auto index = pc - 1;
        if (old_to_new[index].has_value()) {
            next_survivor = old_to_new[index];
        } else {
            old_to_new[index] = next_survivor;
        }
    }

    std::vector<Instruction> code;
    code.reserve(new_size);
    std::vector<DebugSourcePosition> source_positions;
    if (complete_debug_table) {
        source_positions.reserve(new_size);
    }
    for (const auto& replacement : rewrite) {
        if (!replacement.has_value()) {
            continue;
        }
        code.push_back(replacement->instruction);
        if (complete_debug_table) {
            assert(replacement->source_pc <
                   function.source_positions.size());
            source_positions.push_back(
                function.source_positions[replacement->source_pc]);
        }
    }

    for (auto& instruction : code) {
        if (!is_branch(instruction.op)) {
            continue;
        }
        assert(instruction.operand >= 0);
        const auto old_target =
            static_cast<std::size_t>(instruction.operand);
        assert(old_target < old_to_new.size() &&
               old_to_new[old_target].has_value());
        instruction.operand =
            static_cast<std::int64_t>(*old_to_new[old_target]);
    }

    for (auto& handler : function.exception_handlers) {
        assert(old_to_new[handler.try_begin].has_value());
        assert(old_to_new[handler.try_end].has_value());
        assert(old_to_new[handler.target].has_value());
        handler.try_begin = *old_to_new[handler.try_begin];
        handler.try_end = *old_to_new[handler.try_end];
        handler.target = *old_to_new[handler.target];
    }

    function.code = std::move(code);
    if (complete_debug_table) {
        function.source_positions = std::move(source_positions);
    } else {
        function.source_positions.clear();
    }
    function.stack_maps.clear();
}

bool fold_constants_once(Function& function, OptimizationStats& stats) {
    const auto protected_pc = protected_entries(function);
    auto rewrite = identity_rewrite(function);
    bool changed = false;

    for (std::size_t pc = 0; pc < function.code.size();) {
        if (pc + 2 < function.code.size() &&
            function.code[pc].op == OpCode::ConstantI64 &&
            function.code[pc + 1].op == OpCode::ConstantI64 &&
            !protected_pc[pc + 1] && !protected_pc[pc + 2]) {
            const auto left = function.code[pc].operand;
            const auto right = function.code[pc + 1].operand;
            const auto operation = function.code[pc + 2].op;
            std::optional<Instruction> folded;
            if (operation == OpCode::AddI64) {
                folded = Instruction{
                    OpCode::ConstantI64,
                    detail::wrapping_add_i64(left, right)};
            } else if (operation == OpCode::LessI64) {
                folded = Instruction{
                    OpCode::ConstantBool, left < right ? 1 : 0};
            }
            if (folded.has_value()) {
                rewrite[pc] =
                    RewrittenInstruction{*folded, pc + 2};
                rewrite[pc + 1].reset();
                rewrite[pc + 2].reset();
                ++stats.folds_applied;
                changed = true;
                pc += 3;
                continue;
            }
        }

        if (pc + 1 < function.code.size() &&
            function.code[pc].op == OpCode::ConstantI64 &&
            function.code[pc + 1].op == OpCode::I64Abs &&
            !protected_pc[pc + 1]) {
            const auto value = function.code[pc].operand;
            if (value != std::numeric_limits<std::int64_t>::min()) {
                rewrite[pc] = RewrittenInstruction{
                    Instruction{
                        OpCode::ConstantI64,
                        value < 0 ? -value : value},
                    pc + 1};
                rewrite[pc + 1].reset();
                ++stats.folds_applied;
                changed = true;
                pc += 2;
                continue;
            }
        }

        if (pc + 1 < function.code.size() &&
            function.code[pc].op == OpCode::Nil &&
            function.code[pc + 1].op == OpCode::IsNil &&
            !protected_pc[pc + 1]) {
            rewrite[pc] = RewrittenInstruction{
                Instruction{OpCode::ConstantBool, 1}, pc + 1};
            rewrite[pc + 1].reset();
            ++stats.folds_applied;
            changed = true;
            pc += 2;
            continue;
        }

        ++pc;
    }

    if (changed) {
        apply_rewrite(function, rewrite);
    }
    return changed;
}

void fold_constants(Function& function, OptimizationStats& stats) {
    while (fold_constants_once(function, stats)) {
    }
}

bool simplify_constant_branches(Function& function) {
    if (!function.exception_handlers.empty()) {
        return false;
    }

    const auto protected_pc = protected_entries(function);
    auto rewrite = identity_rewrite(function);
    bool changed = false;
    for (std::size_t pc = 0; pc + 1 < function.code.size();) {
        if (function.code[pc].op != OpCode::ConstantBool ||
            function.code[pc + 1].op != OpCode::JumpIfFalse ||
            protected_pc[pc + 1]) {
            ++pc;
            continue;
        }

        if (function.code[pc].operand == 0) {
            rewrite[pc] = RewrittenInstruction{
                Instruction{OpCode::Jump,
                            function.code[pc + 1].operand},
                pc + 1};
            rewrite[pc + 1].reset();
        } else {
            rewrite[pc].reset();
            rewrite[pc + 1].reset();
        }
        changed = true;
        pc += 2;
    }

    if (changed) {
        apply_rewrite(function, rewrite);
    }
    return changed;
}

std::vector<bool> reachable_instructions(const Function& function) {
    std::vector<bool> reachable(function.code.size(), false);
    std::deque<std::size_t> worklist;
    reachable.front() = true;
    worklist.push_back(0);

    const auto enqueue = [&](std::size_t pc) {
        if (!reachable[pc]) {
            reachable[pc] = true;
            worklist.push_back(pc);
        }
    };

    while (!worklist.empty()) {
        const auto pc = worklist.front();
        worklist.pop_front();
        const auto& instruction = function.code[pc];
        if (instruction.op == OpCode::Jump) {
            enqueue(static_cast<std::size_t>(instruction.operand));
            continue;
        }
        if (instruction.op == OpCode::JumpIfFalse) {
            enqueue(static_cast<std::size_t>(instruction.operand));
            enqueue(pc + 1);
            continue;
        }
        if (!is_terminal(instruction.op)) {
            enqueue(pc + 1);
        }
    }
    return reachable;
}

std::vector<bool> basic_block_leaders(const Function& function) {
    std::vector<bool> leaders(function.code.size(), false);
    leaders.front() = true;
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        const auto& instruction = function.code[pc];
        if (is_branch(instruction.op)) {
            leaders[static_cast<std::size_t>(instruction.operand)] = true;
        }
        if ((is_branch(instruction.op) ||
             is_terminal(instruction.op)) &&
            pc + 1 < function.code.size()) {
            leaders[pc + 1] = true;
        }
    }
    for (const auto& handler : function.exception_handlers) {
        leaders[handler.try_begin] = true;
        leaders[handler.try_end] = true;
        leaders[handler.target] = true;
    }
    return leaders;
}

void eliminate_unreachable_code(Function& function,
                                OptimizationStats& stats) {
    if (!function.exception_handlers.empty()) {
        return;
    }

    (void)simplify_constant_branches(function);
    const auto reachable = reachable_instructions(function);
    const auto leaders = basic_block_leaders(function);
    bool changed = false;
    auto rewrite = identity_rewrite(function);
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        if (reachable[pc]) {
            continue;
        }
        changed = true;
        rewrite[pc].reset();
        if (leaders[pc]) {
            ++stats.blocks_eliminated;
        }
    }
    if (changed) {
        apply_rewrite(function, rewrite);
    }
}

struct JumpResolution {
    std::size_t target{0};
    bool cycle{false};
};

JumpResolution resolve_jump_chain(const Function& function,
                                  std::size_t target) {
    std::vector<bool> visited(function.code.size(), false);
    while (function.code[target].op == OpCode::Jump) {
        if (visited[target]) {
            return JumpResolution{target, true};
        }
        visited[target] = true;
        target =
            static_cast<std::size_t>(function.code[target].operand);
    }
    return JumpResolution{target, false};
}

bool thread_jump_chains(Function& function,
                        OptimizationStats& stats) {
    bool changed = false;
    for (auto& instruction : function.code) {
        if (!is_branch(instruction.op)) {
            continue;
        }
        const auto original =
            static_cast<std::size_t>(instruction.operand);
        const auto resolved =
            resolve_jump_chain(function, original);
        if (resolved.cycle || resolved.target == original) {
            continue;
        }
        instruction.operand =
            static_cast<std::int64_t>(resolved.target);
        ++stats.peepholes_applied;
        changed = true;
    }
    return changed;
}

bool remove_identity_peepholes_once(Function& function,
                                    OptimizationStats& stats) {
    const auto protected_pc = protected_entries(function);
    auto rewrite = identity_rewrite(function);
    bool changed = false;

    for (std::size_t pc = 0; pc < function.code.size();) {
        if (pc + 1 < function.code.size() &&
            function.code[pc].op == OpCode::ConstantI64 &&
            function.code[pc].operand == 0 &&
            function.code[pc + 1].op == OpCode::AddI64 &&
            !protected_pc[pc + 1]) {
            rewrite[pc].reset();
            rewrite[pc + 1].reset();
            ++stats.peepholes_applied;
            changed = true;
            pc += 2;
            continue;
        }

        if (function.code[pc].op == OpCode::Jump &&
            function.code[pc].operand ==
                static_cast<std::int64_t>(pc + 1)) {
            const auto resolved =
                resolve_jump_chain(
                    function,
                    static_cast<std::size_t>(
                        function.code[pc].operand));
            if (!resolved.cycle) {
                rewrite[pc].reset();
                ++stats.peepholes_applied;
                changed = true;
            }
        }
        ++pc;
    }

    if (changed) {
        apply_rewrite(function, rewrite);
    }
    return changed;
}

void apply_peepholes(Function& function,
                     OptimizationStats& stats) {
    bool changed = true;
    while (changed) {
        changed = thread_jump_chains(function, stats);
        changed =
            remove_identity_peepholes_once(function, stats) ||
            changed;
    }
}

OptimizationResult optimize_impl(Module module, OptimizerOptions options,
                                 bool corrupt_output) {
    OptimizationResult result;
    result.stats.instructions_before = instruction_count(module);

    auto input_report = verify_with_diagnostics(module);
    if (!input_report.result.has_value()) {
        result.diagnostics = std::move(input_report.diagnostics);
        return result;
    }

    for (auto& function : module.functions) {
        function.stack_maps.clear();
    }

    if (options.constant_folding) {
        for (auto& function : module.functions) {
            fold_constants(function, result.stats);
        }
    }
    if (options.dead_code_elimination) {
        for (auto& function : module.functions) {
            eliminate_unreachable_code(function, result.stats);
        }
    }
    if (options.peephole) {
        for (auto& function : module.functions) {
            apply_peepholes(function, result.stats);
        }
    }

    if (corrupt_output) {
        module.functions[module.entry_function].code.front() =
            Instruction{OpCode::AddI64, 0};
    }

    result.stats.instructions_after = instruction_count(module);
    auto output_report =
        verify_module_with_diagnostics(std::move(module));
    result.diagnostics = std::move(output_report.diagnostics);
    if (output_report.module.has_value()) {
        result.verified_module = std::move(output_report.module);
    }
    return result;
}

} // namespace

OptimizationResult optimize_module(Module module, OptimizerOptions options) {
    return optimize_impl(std::move(module), options, false);
}

OptimizationResult TEST_ONLY_optimize_module_with_invalid_output(
    Module module, OptimizerOptions options) {
    return optimize_impl(std::move(module), options, true);
}

} // namespace lang
