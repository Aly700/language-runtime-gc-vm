#include "compiler.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace lang::frontend::detail {
namespace {

ValueKind bytecode_kind(const TypeSpec& type) {
    switch (type.kind) {
    case TypeSpec::Kind::Int64:
        return ValueKind::Int64;
    case TypeSpec::Kind::Bool:
        return ValueKind::Bool;
    case TypeSpec::Kind::Str:
        return ValueKind::Str;
    case TypeSpec::Kind::Pair:
    case TypeSpec::Kind::Named:
        return ValueKind::Object;
    case TypeSpec::Kind::Array:
        return ValueKind::Array;
    case TypeSpec::Kind::Function:
        return ValueKind::Function;
    case TypeSpec::Kind::Map:
        return ValueKind::Map;
    case TypeSpec::Kind::Weak:
        return ValueKind::Weak;
    case TypeSpec::Kind::Nil:
        return ValueKind::Nil;
    case TypeSpec::Kind::Invalid:
        return ValueKind::Nil;
    }
    return ValueKind::Nil;
}

SignatureValue signature_value_from_type(const TypeSpec& type) {
    if (type.kind == TypeSpec::Kind::Named && type.named_type_index.has_value()) {
        return named_type_signature(*type.named_type_index);
    }
    if (type.has_pair_fields()) {
        return pair_signature(signature_value_from_type(*type.left),
                              signature_value_from_type(*type.right));
    }
    if (type.kind == TypeSpec::Kind::Array && type.element != nullptr) {
        return array_signature(signature_value_from_type(*type.element));
    }
    if (type.kind == TypeSpec::Kind::Function &&
        type.function_return != nullptr) {
        std::vector<SignatureValue> parameters;
        parameters.reserve(type.function_parameters.size());
        for (const auto& parameter : type.function_parameters) {
            parameters.push_back(signature_value_from_type(parameter));
        }
        return function_signature(
            std::move(parameters),
            signature_value_from_type(*type.function_return));
    }
    if (type.kind == TypeSpec::Kind::Map && type.key != nullptr &&
        type.value != nullptr) {
        assert((type.key->kind == TypeSpec::Kind::Int64 ||
                type.key->kind == TypeSpec::Kind::Bool ||
                type.key->kind == TypeSpec::Kind::Str) &&
               "compile boundary rejected an invalid map key type");
        return map_signature(signature_value_from_type(*type.key),
                             signature_value_from_type(*type.value));
    }
    if (type.kind == TypeSpec::Kind::Weak &&
        type.weak_target != nullptr) {
        return weak_signature(
            signature_value_from_type(*type.weak_target));
    }
    return signature_value(bytecode_kind(type));
}

bool is_scalar_array_element_type(const TypeSpec& type) {
    return type.kind == TypeSpec::Kind::Int64 || type.kind == TypeSpec::Kind::Bool;
}

bool is_ref_array_type(const TypeSpec& array_type) {
    return array_type.kind == TypeSpec::Kind::Array &&
           array_type.element != nullptr &&
           !is_scalar_array_element_type(*array_type.element);
}

struct ArrayOpcodeCounts {
    std::size_t alloc_scalar{0};
    std::size_t alloc_ref{0};
    std::size_t get_scalar{0};
    std::size_t get_ref{0};
    std::size_t set_scalar{0};
    std::size_t set_ref{0};
};

bool operator==(const ArrayOpcodeCounts& lhs, const ArrayOpcodeCounts& rhs) {
    return lhs.alloc_scalar == rhs.alloc_scalar &&
           lhs.alloc_ref == rhs.alloc_ref &&
           lhs.get_scalar == rhs.get_scalar &&
           lhs.get_ref == rhs.get_ref &&
           lhs.set_scalar == rhs.set_scalar &&
           lhs.set_ref == rhs.set_ref;
}

void add_expr_array_counts(const Expr& expression, ArrayOpcodeCounts& counts);

bool add_statement_array_counts(const Statement& statement,
                                ArrayOpcodeCounts& counts);

std::pair<bool, bool> block_fallthrough_and_current_break(
    const std::vector<Statement>& statements) {
    bool falls_through = true;
    bool has_break = false;
    for (const auto& statement : statements) {
        if (!falls_through) {
            break;
        }
        switch (statement.kind) {
        case Statement::Kind::Break:
            has_break = true;
            falls_through = false;
            break;
        case Statement::Kind::Continue:
            falls_through = false;
            break;
        case Statement::Kind::If: {
            const auto then_flow =
                block_fallthrough_and_current_break(statement.then_branch);
            const auto else_flow =
                block_fallthrough_and_current_break(statement.else_branch);
            has_break = has_break || then_flow.second || else_flow.second;
            falls_through = then_flow.first || else_flow.first;
            break;
        }
        case Statement::Kind::Let:
        case Statement::Kind::Assign:
        case Statement::Kind::While:
        case Statement::Kind::ForIn:
        case Statement::Kind::Print:
            break;
        }
    }
    return {falls_through, has_break};
}

bool add_block_array_counts(const std::vector<Statement>& statements,
                            ArrayOpcodeCounts& counts) {
    bool falls_through = true;
    for (const auto& statement : statements) {
        if (!falls_through) {
            break;
        }
        falls_through = add_statement_array_counts(statement, counts);
    }
    return falls_through;
}

void add_lvalue_prefix_array_counts(const LValue& lvalue,
                                    std::size_t step_count,
                                    ArrayOpcodeCounts& counts) {
    for (std::size_t i = 0; i < step_count; ++i) {
        const auto& step = lvalue.steps[i];
        if (step.kind == LValueStep::Kind::Index) {
            add_expr_array_counts(*step.index, counts);
            if (step.receiver_type.kind == TypeSpec::Kind::Map) {
                continue;
            }
            if (is_ref_array_type(step.receiver_type)) {
                ++counts.get_ref;
            } else {
                ++counts.get_scalar;
            }
        }
    }
}

bool add_statement_array_counts(const Statement& statement,
                                ArrayOpcodeCounts& counts) {
    switch (statement.kind) {
    case Statement::Kind::Let:
        add_expr_array_counts(*statement.initializer, counts);
        return true;
    case Statement::Kind::Assign:
        add_expr_array_counts(*statement.value, counts);
        if (!statement.target.steps.empty()) {
            const auto& final_step = statement.target.steps.back();
            add_lvalue_prefix_array_counts(statement.target,
                                           statement.target.steps.size() - 1,
                                           counts);
            if (final_step.kind == LValueStep::Kind::Index) {
                add_expr_array_counts(*final_step.index, counts);
                if (statement.target.receiver_type.kind ==
                    TypeSpec::Kind::Map) {
                    break;
                }
                if (is_ref_array_type(statement.target.receiver_type)) {
                    ++counts.set_ref;
                } else {
                    ++counts.set_scalar;
                }
            }
        }
        return true;
    case Statement::Kind::If:
        add_expr_array_counts(*statement.condition, counts);
        {
            const bool then_falls =
                add_block_array_counts(statement.then_branch, counts);
            const bool else_falls =
                add_block_array_counts(statement.else_branch, counts);
            return then_falls || else_falls;
        }
    case Statement::Kind::While:
        add_expr_array_counts(*statement.condition, counts);
        (void)add_block_array_counts(statement.body, counts);
        return statement.condition->kind != Expr::Kind::BoolLiteral ||
               !statement.condition->bool_value ||
               block_fallthrough_and_current_break(statement.body).second;
    case Statement::Kind::ForIn:
        add_expr_array_counts(*statement.iterable, counts);
        if (statement.range_upper != nullptr) {
            add_expr_array_counts(*statement.range_upper, counts);
        }
        (void)add_block_array_counts(statement.body, counts);
        if (statement.range_upper == nullptr &&
            statement.iterable->inferred_type.kind == TypeSpec::Kind::Array) {
            if (is_ref_array_type(statement.iterable->inferred_type)) {
                ++counts.get_ref;
            } else {
                ++counts.get_scalar;
            }
        }
        return true;
    case Statement::Kind::Break:
    case Statement::Kind::Continue:
        return false;
    case Statement::Kind::Print:
        add_expr_array_counts(*statement.value, counts);
        return true;
    }
    return true;
}

void add_expr_array_counts(const Expr& expression, ArrayOpcodeCounts& counts) {
    switch (expression.kind) {
    case Expr::Kind::IntLiteral:
    case Expr::Kind::BoolLiteral:
    case Expr::Kind::StringLiteral:
    case Expr::Kind::NilLiteral:
    case Expr::Kind::Variable:
    case Expr::Kind::MapEmpty:
        return;
    case Expr::Kind::Lambda:
        assert(expression.lambda != nullptr);
        if (add_block_array_counts(expression.lambda->statements, counts)) {
            add_expr_array_counts(*expression.lambda->result, counts);
        }
        return;
    case Expr::Kind::PairLiteral:
    case Expr::Kind::Binary:
        add_expr_array_counts(*expression.left, counts);
        add_expr_array_counts(*expression.right, counts);
        return;
    case Expr::Kind::ArrayLiteral:
        for (const auto& argument : expression.arguments) {
            add_expr_array_counts(*argument, counts);
        }
        if (is_ref_array_type(expression.inferred_type)) {
            ++counts.alloc_ref;
            counts.set_ref += expression.arguments.empty() ? 0
                                                           : expression.arguments.size() - 1;
        } else {
            ++counts.alloc_scalar;
            counts.set_scalar += expression.arguments.empty() ? 0
                                                              : expression.arguments.size() - 1;
        }
        return;
    case Expr::Kind::ArraySized:
        add_expr_array_counts(*expression.left, counts);
        add_expr_array_counts(*expression.right, counts);
        if (is_scalar_array_element_type(expression.array_element_type)) {
            ++counts.alloc_scalar;
        } else {
            ++counts.alloc_ref;
        }
        return;
    case Expr::Kind::ArrayIndex:
        add_expr_array_counts(*expression.receiver, counts);
        add_expr_array_counts(*expression.left, counts);
        if (expression.receiver->inferred_type.kind == TypeSpec::Kind::Str) {
            return;
        }
        if (expression.receiver->inferred_type.kind == TypeSpec::Kind::Map) {
            return;
        }
        if (is_ref_array_type(expression.receiver->inferred_type)) {
            ++counts.get_ref;
        } else {
            ++counts.get_scalar;
        }
        return;
    case Expr::Kind::ArrayLen:
        add_expr_array_counts(*expression.receiver, counts);
        return;
    case Expr::Kind::Field:
    case Expr::Kind::IsNil:
    case Expr::Kind::WeakConstruct:
    case Expr::Kind::WeakGet:
    case Expr::Kind::ToStr:
    case Expr::Kind::ToI64:
        add_expr_array_counts(*expression.receiver, counts);
        return;
    case Expr::Kind::MapHas:
        add_expr_array_counts(*expression.receiver, counts);
        add_expr_array_counts(*expression.left, counts);
        return;
    case Expr::Kind::Call:
        for (const auto& argument : expression.arguments) {
            add_expr_array_counts(*argument, counts);
        }
        if (!expression.direct_call) {
            add_expr_array_counts(*expression.receiver, counts);
        }
        return;
    }
}

ArrayOpcodeCounts expected_array_opcode_counts(const Program& program) {
    ArrayOpcodeCounts counts;
    if (add_block_array_counts(program.statements, counts)) {
        add_expr_array_counts(*program.result, counts);
    }
    for (const auto& function : program.functions) {
        if (add_block_array_counts(function.statements, counts)) {
            add_expr_array_counts(*function.result, counts);
        }
    }
    return counts;
}

ArrayOpcodeCounts actual_array_opcode_counts(const Module& module) {
    ArrayOpcodeCounts counts;
    for (const auto& function : module.functions) {
        for (const auto& instruction : function.code) {
            switch (instruction.op) {
            case OpCode::AllocArray:
                ++counts.alloc_scalar;
                break;
            case OpCode::AllocRefArray:
                ++counts.alloc_ref;
                break;
            case OpCode::ArrayGet:
                ++counts.get_scalar;
                break;
            case OpCode::RefArrayGet:
                ++counts.get_ref;
                break;
            case OpCode::ArraySet:
                ++counts.set_scalar;
                break;
            case OpCode::RefArraySet:
                ++counts.set_ref;
                break;
            default:
                break;
            }
        }
    }
    return counts;
}

FunctionSignature signature_from_types(const std::vector<Parameter>& parameters,
                                       const TypeSpec& return_type) {
    FunctionSignature signature;
    signature.parameters.reserve(parameters.size());
    signature.parameter_types.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        signature.parameters.push_back(bytecode_kind(parameter.type));
        signature.parameter_types.push_back(signature_value_from_type(parameter.type));
    }
    signature.return_type = bytecode_kind(return_type);
    signature.return_type_detail = signature_value_from_type(return_type);
    assert(signature.parameter_types.size() == signature.parameters.size());
    assert(signature.return_type_detail->kind == signature.return_type);
    return signature;
}

SignatureValue closure_function_type_from_types(
    const std::vector<Parameter>& parameters, const TypeSpec& return_type) {
    std::vector<SignatureValue> parameter_types;
    parameter_types.reserve(parameters.size());
    for (const auto& parameter : parameters) {
        parameter_types.push_back(signature_value_from_type(parameter.type));
    }
    return function_signature(std::move(parameter_types),
                              signature_value_from_type(return_type));
}

class Compiler {
public:
    Compiler(std::uint32_t local_count, FunctionSignature signature,
             std::vector<std::string>& string_constants,
             std::vector<MapLayout>& map_layouts,
             std::optional<std::size_t> closure_layout = std::nullopt)
        : string_constants_(string_constants), map_layouts_(map_layouts) {
        function_.signature = std::move(signature);
        function_.local_count = local_count;
        function_.closure_layout = closure_layout;
    }

    Function compile(const std::vector<Statement>& statements, const Expr& result) {
        bool falls_through = true;
        for (const auto& statement : statements) {
            if (!falls_through) {
                break;
            }
            falls_through = compile_statement(statement);
        }
        if (falls_through) {
            compile_expr(result);
            // Verifier accommodation: source programs always end in a final expression,
            // and the compiler emits an explicit Return immediately after it so bytecode cannot
            // fall off the end.
            emit(OpCode::Return, 0);
        }
        return function_;
    }

private:
    std::size_t emit(OpCode op, std::int64_t operand) {
        function_.code.push_back(Instruction{op, operand});
        return function_.code.size() - 1;
    }

    void patch(std::size_t instruction, std::size_t target) {
        assert(instruction < function_.code.size());
        assert(target <= static_cast<std::size_t>(std::numeric_limits<std::int64_t>::max()));
        function_.code[instruction].operand = static_cast<std::int64_t>(target);
    }

    std::size_t pc() const { return function_.code.size(); }

    std::uint32_t allocate_temp_local() {
        return function_.local_count++;
    }

    struct LoopPatchContext {
        std::vector<std::size_t> breaks;
        std::vector<std::size_t> continues;
    };

    bool compile_block(const std::vector<Statement>& statements) {
        bool falls_through = true;
        for (const auto& statement : statements) {
            if (!falls_through) {
                break;
            }
            falls_through = compile_statement(statement);
        }
        return falls_through;
    }

    bool compile_statement(const Statement& statement) {
        switch (statement.kind) {
        case Statement::Kind::Let:
            // Verifier accommodation: every source local is introduced by a top-level
            // let with an initializer, and the initializer is stored before any later
            // LoadLocal can be emitted for that local.
            compile_expr(*statement.initializer);
            emit(OpCode::StoreLocal, statement.local_index);
            return true;
        case Statement::Kind::Assign:
            compile_assignment(statement);
            return true;
        case Statement::Kind::If:
            return compile_if(statement);
        case Statement::Kind::While:
            return compile_while(statement);
        case Statement::Kind::ForIn:
            compile_for_in(statement);
            return true;
        case Statement::Kind::Break:
        case Statement::Kind::Continue: {
            assert(!loop_contexts_.empty() &&
                   "type checker must reject loop control outside loops");
            const auto jump = emit(OpCode::Jump, -1);
            if (statement.kind == Statement::Kind::Break) {
                loop_contexts_.back()->breaks.push_back(jump);
            } else {
                loop_contexts_.back()->continues.push_back(jump);
            }
            return false;
        }
        case Statement::Kind::Print:
            compile_expr(*statement.value);
            emit(OpCode::Print, 0);
            return true;
        }
        return true;
    }

    void compile_assignment(const Statement& statement) {
        if (statement.target.steps.empty()) {
            compile_expr(*statement.value);
            emit(OpCode::StoreLocal, statement.target.local_index);
            return;
        }

        const auto& final_step = statement.target.steps.back();
        compile_lvalue_receiver(statement.target);
        if (final_step.kind == LValueStep::Kind::Index) {
            compile_expr(*final_step.index);
            if (statement.target.receiver_type.kind == TypeSpec::Kind::Map) {
                compile_expr(*statement.value);
                emit(OpCode::MapSet, 0);
                return;
            }
            compile_expr_as_array_storage(*statement.value,
                                          statement.target.element_type);
            emit(is_ref_array_type(statement.target.receiver_type)
                     ? OpCode::RefArraySet
                     : OpCode::ArraySet,
                 0);
            return;
        }

        // Verifier accommodation: SetLeft/SetRight consumes receiver then value and leaves
        // no stack result, so field-assignment statements compile as stack-neutral blocks.
        compile_expr(*statement.value);
        emit(final_step.name == "left" ? OpCode::SetLeft : OpCode::SetRight, 0);
    }

    bool compile_if(const Statement& statement) {
        compile_expr(*statement.condition);
        const auto jump_to_else = emit(OpCode::JumpIfFalse, -1);
        const bool then_falls = compile_block(statement.then_branch);
        const auto jump_to_end =
            then_falls ? std::optional<std::size_t>(emit(OpCode::Jump, -1))
                       : std::nullopt;
        const auto else_pc = pc();
        patch(jump_to_else, else_pc);
        const bool else_falls = compile_block(statement.else_branch);
        if (jump_to_end.has_value()) {
            patch(*jump_to_end, pc());
        }
        return then_falls || else_falls;
    }

    bool compile_while(const Statement& statement) {
        const bool condition_is_true_literal =
            statement.condition->kind == Expr::Kind::BoolLiteral &&
            statement.condition->bool_value;
        const auto header = pc();
        std::optional<std::size_t> jump_to_exit;
        if (!condition_is_true_literal) {
            compile_expr(*statement.condition);
            jump_to_exit = emit(OpCode::JumpIfFalse, -1);
        }
        LoopPatchContext loop;
        loop_contexts_.push_back(&loop);
        const bool body_falls = compile_block(statement.body);
        loop_contexts_.pop_back();
        for (const auto continued : loop.continues) {
            patch(continued, header);
        }
        // Verifier accommodation: loop bodies are stack-neutral and locals keep their
        // declared source type, matching the verifier's strict merge at the backedge.
        if (body_falls) {
            emit(OpCode::Jump, static_cast<std::int64_t>(header));
        }
        const auto exit = pc();
        if (jump_to_exit.has_value()) {
            patch(*jump_to_exit, exit);
        }
        for (const auto broken : loop.breaks) {
            patch(broken, exit);
        }
        return !condition_is_true_literal || !loop.breaks.empty();
    }

    void emit_increment_local(std::uint32_t local) {
        emit(OpCode::LoadLocal, local);
        emit(OpCode::ConstantI64, 1);
        emit(OpCode::AddI64, 0);
        emit(OpCode::StoreLocal, local);
    }

    void compile_for_in(const Statement& statement) {
        assert(!statement.loop_local_indices.empty() &&
               "type-checked for-in must own loop locals");
        const auto index = allocate_temp_local();
        const auto bound = allocate_temp_local();

        if (statement.range_upper != nullptr) {
            // Range bounds are evaluated exactly once, left-to-right. The lower value is
            // the compiler-managed induction local and the upper value is its fixed bound.
            compile_expr(*statement.iterable);
            emit(OpCode::StoreLocal, index);
            compile_expr(*statement.range_upper);
            emit(OpCode::StoreLocal, bound);

            const auto header = pc();
            emit(OpCode::LoadLocal, index);
            emit(OpCode::LoadLocal, bound);
            emit(OpCode::LessI64, 0);
            const auto jump_to_exit = emit(OpCode::JumpIfFalse, -1);
            emit(OpCode::LoadLocal, index);
            emit(OpCode::StoreLocal, statement.loop_local_indices[0]);
            LoopPatchContext loop;
            loop_contexts_.push_back(&loop);
            const bool body_falls = compile_block(statement.body);
            loop_contexts_.pop_back();
            const auto increment = pc();
            for (const auto continued : loop.continues) {
                patch(continued, increment);
            }
            if (body_falls || !loop.continues.empty()) {
                emit_increment_local(index);
                emit(OpCode::Jump, static_cast<std::int64_t>(header));
            }
            const auto exit = pc();
            patch(jump_to_exit, exit);
            for (const auto broken : loop.breaks) {
                patch(broken, exit);
            }
            return;
        }

        const auto container = allocate_temp_local();
        compile_expr(*statement.iterable);
        emit(OpCode::StoreLocal, container);
        emit(OpCode::LoadLocal, container);
        emit(statement.iterable->inferred_type.kind == TypeSpec::Kind::Map
                 ? OpCode::MapLen
                 : OpCode::ArrayLen,
             0);
        emit(OpCode::StoreLocal, bound);
        emit(OpCode::ConstantI64, 0);
        emit(OpCode::StoreLocal, index);

        const auto header = pc();
        if (statement.iterable->inferred_type.kind == TypeSpec::Kind::Map) {
            // Maps cannot shrink. A larger current count therefore proves a new key was
            // inserted. The deliberately invalid positional read provides one stable,
            // deterministic runtime trap using the accessor's ordinary OOB boundary.
            emit(OpCode::LoadLocal, bound);
            emit(OpCode::LoadLocal, container);
            emit(OpCode::MapLen, 0);
            emit(OpCode::LessI64, 0);
            const auto unchanged = emit(OpCode::JumpIfFalse, -1);
            emit(OpCode::LoadLocal, container);
            emit(OpCode::ConstantI64, -1);
            emit(OpCode::MapKeyAt, 0);
            emit(OpCode::StoreLocal, statement.loop_local_indices[0]);
            patch(unchanged, pc());
        }

        emit(OpCode::LoadLocal, index);
        emit(OpCode::LoadLocal, bound);
        emit(OpCode::LessI64, 0);
        const auto jump_to_exit = emit(OpCode::JumpIfFalse, -1);

        emit(OpCode::LoadLocal, container);
        emit(OpCode::LoadLocal, index);
        if (statement.iterable->inferred_type.kind == TypeSpec::Kind::Map) {
            emit(OpCode::MapKeyAt, 0);
            emit(OpCode::StoreLocal, statement.loop_local_indices[0]);
            emit(OpCode::LoadLocal, container);
            emit(OpCode::LoadLocal, index);
            emit(OpCode::MapValueAt, 0);
            emit(OpCode::StoreLocal, statement.loop_local_indices[1]);
        } else {
            const auto& element = *statement.iterable->inferred_type.element;
            emit(is_scalar_array_element_type(element) ? OpCode::ArrayGet
                                                       : OpCode::RefArrayGet,
                 0);
            if (element.kind == TypeSpec::Kind::Bool) {
                emit(OpCode::ConstantI64, 1);
                emit(OpCode::LessI64, 0);
            }
            emit(OpCode::StoreLocal, statement.loop_local_indices[0]);
        }
        LoopPatchContext loop;
        loop_contexts_.push_back(&loop);
        const bool body_falls = compile_block(statement.body);
        loop_contexts_.pop_back();
        const auto increment = pc();
        for (const auto continued : loop.continues) {
            patch(continued, increment);
        }
        if (body_falls || !loop.continues.empty()) {
            emit_increment_local(index);
            emit(OpCode::Jump, static_cast<std::int64_t>(header));
        }
        const auto exit = pc();
        patch(jump_to_exit, exit);
        for (const auto broken : loop.breaks) {
            patch(broken, exit);
        }
    }

    void compile_expr(const Expr& expression) {
        switch (expression.kind) {
        case Expr::Kind::IntLiteral:
            emit(OpCode::ConstantI64, expression.int_value);
            break;
        case Expr::Kind::BoolLiteral:
            compile_bool_literal(expression.bool_value);
            break;
        case Expr::Kind::StringLiteral:
            assert(string_constants_.size() <=
                       static_cast<std::size_t>(
                           std::numeric_limits<std::int64_t>::max()) &&
                   "module string constant pool index exceeds bytecode operand range");
            emit(OpCode::PushStr,
                 static_cast<std::int64_t>(string_constants_.size()));
            string_constants_.push_back(expression.string_value);
            break;
        case Expr::Kind::NilLiteral:
            emit(OpCode::Nil, 0);
            break;
        case Expr::Kind::Variable:
            if (expression.is_capture) {
                emit(OpCode::LoadCapture,
                     static_cast<std::int64_t>(expression.capture_index));
            } else if (expression.is_function_reference) {
                emit(OpCode::AllocClosure,
                     static_cast<std::int64_t>(
                         expression.closure_layout_index));
            } else {
                emit(OpCode::LoadLocal, expression.local_index);
            }
            break;
        case Expr::Kind::Lambda:
            compile_lambda(expression);
            break;
        case Expr::Kind::PairLiteral:
            compile_expr(*expression.left);
            compile_expr(*expression.right);
            emit(OpCode::AllocPair, 0);
            break;
        case Expr::Kind::ArrayLiteral:
            compile_array_literal(expression);
            break;
        case Expr::Kind::ArraySized:
            compile_array_sized(expression);
            break;
        case Expr::Kind::ArrayIndex:
            compile_array_index(expression);
            break;
        case Expr::Kind::ArrayLen:
            compile_expr(*expression.receiver);
            if (expression.receiver->inferred_type.kind == TypeSpec::Kind::Str) {
                emit(OpCode::StrLen, 0);
            } else if (expression.receiver->inferred_type.kind ==
                       TypeSpec::Kind::Map) {
                emit(OpCode::MapLen, 0);
            } else {
                emit(OpCode::ArrayLen, 0);
            }
            break;
        case Expr::Kind::Binary:
            compile_expr(*expression.left);
            compile_expr(*expression.right);
            if (expression.binary_op == '+') {
                emit(expression.left->inferred_type.kind == TypeSpec::Kind::Str
                         ? OpCode::StrConcat
                         : OpCode::AddI64,
                     0);
            } else if (expression.binary_op == '<') {
                emit(OpCode::LessI64, 0);
            } else {
                emit(OpCode::StrEq, 0);
                if (expression.binary_op == '!') {
                    invert_bool_on_stack();
                }
            }
            break;
        case Expr::Kind::Field:
            compile_expr(*expression.receiver);
            emit(expression.name == "left" ? OpCode::GetLeft : OpCode::GetRight, 0);
            break;
        case Expr::Kind::Call:
            for (const auto& argument : expression.arguments) {
                compile_expr(*argument);
            }
            if (expression.direct_call) {
                emit(OpCode::Call,
                     static_cast<std::int64_t>(expression.callee_index));
            } else {
                compile_expr(*expression.receiver);
                emit(OpCode::CallClosure, 0);
            }
            break;
        case Expr::Kind::IsNil:
            compile_expr(*expression.receiver);
            emit(OpCode::IsNil, 0);
            break;
        case Expr::Kind::MapEmpty:
            compile_map_empty(expression);
            break;
        case Expr::Kind::MapHas:
            compile_expr(*expression.receiver);
            compile_expr(*expression.left);
            emit(OpCode::MapHas, 0);
            break;
        case Expr::Kind::WeakConstruct:
            compile_expr(*expression.receiver);
            emit(OpCode::AllocWeak, 0);
            break;
        case Expr::Kind::WeakGet:
            compile_expr(*expression.receiver);
            emit(OpCode::WeakGet, 0);
            break;
        case Expr::Kind::ToStr:
            compile_expr(*expression.receiver);
            emit(expression.receiver->inferred_type.kind == TypeSpec::Kind::Bool
                     ? OpCode::BoolToStr
                     : OpCode::I64ToStr,
                 0);
            break;
        case Expr::Kind::ToI64:
            compile_expr(*expression.receiver);
            emit(OpCode::StrToI64, 0);
            break;
        }
    }

    void compile_bool_literal(bool value) {
        // Verifier accommodation: the VM has no Bool literal opcode. Emitting a constant
        // comparison lets the verifier prove the result kind is Bool before JumpIfFalse or
        // StoreLocal consumes it.
        emit(OpCode::ConstantI64, value ? 0 : 1);
        emit(OpCode::ConstantI64, value ? 1 : 0);
        emit(OpCode::LessI64, 0);
    }

    void compile_map_empty(const Expr& expression) {
        assert((expression.map_key_type.kind == TypeSpec::Kind::Int64 ||
                expression.map_key_type.kind == TypeSpec::Kind::Bool ||
                expression.map_key_type.kind == TypeSpec::Kind::Str) &&
               "compile boundary must enforce the map key restriction");
        auto key = signature_value_from_type(expression.map_key_type);
        auto value = signature_value_from_type(expression.map_value_type);
        assert(map_layouts_.size() <=
                   static_cast<std::size_t>(
                       std::numeric_limits<std::int64_t>::max()) &&
               "module map layout index exceeds bytecode operand range");
        const auto layout_index = map_layouts_.size();
        map_layouts_.push_back(MapLayout{key, value,
                                         signature_value_is_reference(key),
                                         signature_value_is_reference(value)});
        emit(OpCode::AllocMap, static_cast<std::int64_t>(layout_index));
    }

    void compile_lambda(const Expr& expression) {
        assert(expression.lambda != nullptr &&
               "type-checked lambda must carry body metadata");
        for (const auto& capture : expression.lambda->captures) {
            if (capture.source_is_capture) {
                emit(OpCode::LoadCapture, capture.source_index);
            } else {
                emit(OpCode::LoadLocal, capture.source_index);
            }
        }
        emit(OpCode::AllocClosure,
             static_cast<std::int64_t>(
                 expression.lambda->closure_layout_index));
    }

    void invert_bool_on_stack() {
        const auto jump_to_true = emit(OpCode::JumpIfFalse, -1);
        compile_bool_literal(false);
        const auto jump_to_end = emit(OpCode::Jump, -1);
        patch(jump_to_true, pc());
        compile_bool_literal(true);
        patch(jump_to_end, pc());
    }

    void compile_bool_as_scalar_storage(const Expr& expression) {
        compile_expr(expression);
        const auto jump_to_false = emit(OpCode::JumpIfFalse, -1);
        emit(OpCode::ConstantI64, 0);
        const auto jump_to_end = emit(OpCode::Jump, -1);
        patch(jump_to_false, pc());
        emit(OpCode::ConstantI64, 1);
        patch(jump_to_end, pc());
    }

    void compile_expr_as_array_storage(const Expr& expression,
                                       const TypeSpec& element_type) {
        if (element_type.kind == TypeSpec::Kind::Bool) {
            compile_bool_as_scalar_storage(expression);
            return;
        }
        compile_expr(expression);
    }

    void compile_array_sized(const Expr& expression) {
        compile_expr(*expression.left);
        compile_expr_as_array_storage(*expression.right,
                                      expression.array_element_type);
        emit(is_scalar_array_element_type(expression.array_element_type)
                 ? OpCode::AllocArray
                 : OpCode::AllocRefArray,
             0);
    }

    void compile_array_literal(const Expr& expression) {
        assert(!expression.arguments.empty() &&
               "type checker rejects empty array literals before codegen");
        assert(expression.inferred_type.kind == TypeSpec::Kind::Array &&
               expression.inferred_type.element != nullptr);
        const auto& element_type = *expression.inferred_type.element;
        emit(OpCode::ConstantI64,
             static_cast<std::int64_t>(expression.arguments.size()));
        compile_expr_as_array_storage(*expression.arguments.front(), element_type);
        emit(is_scalar_array_element_type(element_type) ? OpCode::AllocArray
                                                        : OpCode::AllocRefArray,
             0);
        if (expression.arguments.size() == 1) {
            return;
        }

        const auto temp = allocate_temp_local();
        emit(OpCode::StoreLocal, temp);
        for (std::size_t i = 1; i < expression.arguments.size(); ++i) {
            emit(OpCode::LoadLocal, temp);
            emit(OpCode::ConstantI64, static_cast<std::int64_t>(i));
            compile_expr_as_array_storage(*expression.arguments[i], element_type);
            emit(is_scalar_array_element_type(element_type) ? OpCode::ArraySet
                                                            : OpCode::RefArraySet,
                 0);
        }
        emit(OpCode::LoadLocal, temp);
    }

    void compile_array_index(const Expr& expression) {
        assert(expression.receiver != nullptr);
        compile_expr(*expression.receiver);
        compile_expr(*expression.left);
        if (expression.receiver->inferred_type.kind == TypeSpec::Kind::Str) {
            emit(OpCode::StrIndex, 0);
            return;
        }
        if (expression.receiver->inferred_type.kind == TypeSpec::Kind::Map) {
            emit(OpCode::MapGet, 0);
            return;
        }
        assert(expression.receiver->inferred_type.kind == TypeSpec::Kind::Array &&
               expression.receiver->inferred_type.element != nullptr);
        const auto& element_type = *expression.receiver->inferred_type.element;
        emit(is_scalar_array_element_type(element_type) ? OpCode::ArrayGet
                                                        : OpCode::RefArrayGet,
             0);
        if (element_type.kind == TypeSpec::Kind::Bool) {
            emit(OpCode::ConstantI64, 1);
            emit(OpCode::LessI64, 0);
        }
    }

    void compile_lvalue_receiver(const LValue& lvalue) {
        emit(OpCode::LoadLocal, lvalue.local_index);
        for (std::size_t i = 0; i + 1 < lvalue.steps.size(); ++i) {
            const auto& step = lvalue.steps[i];
            if (step.kind == LValueStep::Kind::Field) {
                emit(step.name == "left" ? OpCode::GetLeft : OpCode::GetRight, 0);
            } else {
                compile_expr(*step.index);
                if (step.receiver_type.kind == TypeSpec::Kind::Map) {
                    emit(OpCode::MapGet, 0);
                } else {
                    emit(is_ref_array_type(step.receiver_type)
                             ? OpCode::RefArrayGet
                             : OpCode::ArrayGet,
                         0);
                }
            }
        }
    }

    Function function_;
    std::vector<LoopPatchContext*> loop_contexts_;
    std::vector<std::string>& string_constants_;
    std::vector<MapLayout>& map_layouts_;
};

} // namespace

CompileModuleResult compile_checked_program(const Program& program,
                                            const TypeSpec& result_type) {
    Module module;
    module.entry_function = 0;
    module.named_types.reserve(program.types.size());
    for (const auto& declaration : program.types) {
        module.named_types.push_back(
            NamedTypeSignature{declaration.name,
                               signature_value_from_type(declaration.body)});
    }
    module.functions.reserve(program.functions.size() + program.lambdas.size() + 1);
    module.closure_layouts.reserve(program.functions.size() +
                                   program.lambdas.size());
    for (const auto& declaration : program.functions) {
        ClosureLayout layout;
        layout.function_index = declaration.function_index;
        layout.function_type = closure_function_type_from_types(
            declaration.parameters, declaration.return_type);
        module.closure_layouts.push_back(std::move(layout));
    }
    for (const auto& lambda : program.lambdas) {
        ClosureLayout layout;
        layout.function_index = lambda->function_index;
        layout.function_type = closure_function_type_from_types(
            lambda->parameters, lambda->return_type);
        layout.capture_types.reserve(lambda->captures.size());
        layout.capture_map.reserve(lambda->captures.size());
        for (const auto& capture : lambda->captures) {
            auto type = signature_value_from_type(capture.type);
            layout.capture_map.push_back(signature_value_is_reference(type));
            layout.capture_types.push_back(std::move(type));
        }
        module.closure_layouts.push_back(std::move(layout));
    }

    std::vector<Function> compiled_functions;
    compiled_functions.reserve(program.functions.size() + program.lambdas.size());
    for (const auto& declaration : program.functions) {
        Compiler function_compiler(declaration.local_count,
                                   signature_from_types(declaration.parameters,
                                                        declaration.return_type),
                                   module.string_constants, module.map_layouts,
                                   declaration.closure_layout_index);
        compiled_functions.push_back(
            function_compiler.compile(declaration.statements, *declaration.result));
    }
    for (const auto& lambda : program.lambdas) {
        Compiler lambda_compiler(
            lambda->local_count,
            signature_from_types(lambda->parameters, lambda->return_type),
            module.string_constants, module.map_layouts,
            lambda->closure_layout_index);
        compiled_functions.push_back(
            lambda_compiler.compile(lambda->statements, *lambda->result));
    }

    FunctionSignature entry_signature;
    entry_signature.return_type = bytecode_kind(result_type);
    entry_signature.return_type_detail = signature_value_from_type(result_type);
    Compiler entry_compiler(program.entry_local_count, std::move(entry_signature),
                            module.string_constants, module.map_layouts);
    module.functions.push_back(entry_compiler.compile(program.statements, *program.result));
    for (auto& function : compiled_functions) {
        module.functions.push_back(std::move(function));
    }

    assert(expected_array_opcode_counts(program) == actual_array_opcode_counts(module) &&
           "compiler bug: source array element types disagreed with scalar/ref array opcodes");

    auto verification_report = verify_module_with_diagnostics(std::move(module));
    if (!verification_report.module.has_value()) {
        for (const auto& diagnostic : verification_report.diagnostics) {
            std::cerr << "compiler verifier diagnostic: "
                      << format_verifier_diagnostic(diagnostic) << "\n";
        }
    }
    assert(verification_report.module.has_value() &&
           "compiler bug: type-checked source emitted verifier-rejected module");
    if (!verification_report.module.has_value()) {
        CompileModuleResult result;
        result.diagnostics = {
            Diagnostic{SourcePosition{}, "compiler emitted verifier-rejected module"}};
        return result;
    }
    auto verified_module = std::move(*verification_report.module);

    auto roundtrip = verify_with_diagnostics(verified_module.module());
    if (!roundtrip.result.has_value()) {
        for (const auto& diagnostic : roundtrip.diagnostics) {
            std::cerr << "compiler stack-map round-trip diagnostic: "
                      << format_verifier_diagnostic(diagnostic) << "\n";
        }
    }
    assert(roundtrip.result.has_value() &&
           "compiler bug: verifier-generated stack maps did not round-trip");

    CompileModuleResult result;
    result.verified_module = std::move(verified_module);
    return result;
}

} // namespace lang::frontend::detail
