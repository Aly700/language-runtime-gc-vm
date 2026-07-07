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
    case TypeSpec::Kind::Pair:
    case TypeSpec::Kind::Named:
        return ValueKind::Object;
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
    return signature_value(bytecode_kind(type));
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

class Compiler {
public:
    Compiler(std::uint32_t local_count, FunctionSignature signature) {
        function_.signature = std::move(signature);
        function_.local_count = local_count;
    }

    Function compile(const std::vector<Statement>& statements, const Expr& result) {
        for (const auto& statement : statements) {
            compile_statement(statement);
        }
        compile_expr(result);
        // Verifier accommodation: source programs always end in a final expression, and
        // the compiler emits an explicit Return immediately after it so bytecode cannot
        // fall off the end.
        emit(OpCode::Return, 0);
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

    void compile_statement(const Statement& statement) {
        switch (statement.kind) {
        case Statement::Kind::Let:
            // Verifier accommodation: every source local is introduced by a top-level
            // let with an initializer, and the initializer is stored before any later
            // LoadLocal can be emitted for that local.
            compile_expr(*statement.initializer);
            emit(OpCode::StoreLocal, statement.local_index);
            break;
        case Statement::Kind::Assign:
            compile_assignment(statement);
            break;
        case Statement::Kind::If:
            compile_if(statement);
            break;
        case Statement::Kind::While:
            compile_while(statement);
            break;
        }
    }

    void compile_assignment(const Statement& statement) {
        if (statement.target.fields.empty()) {
            compile_expr(*statement.value);
            emit(OpCode::StoreLocal, statement.target.local_index);
            return;
        }

        // Verifier accommodation: SetLeft/SetRight consumes receiver then value and leaves
        // no stack result, so field-assignment statements compile as stack-neutral blocks.
        compile_lvalue_receiver(statement.target);
        compile_expr(*statement.value);
        const auto& field = statement.target.fields.back();
        emit(field.name == "left" ? OpCode::SetLeft : OpCode::SetRight, 0);
    }

    void compile_if(const Statement& statement) {
        compile_expr(*statement.condition);
        const auto jump_to_else = emit(OpCode::JumpIfFalse, -1);
        for (const auto& inner : statement.then_branch) {
            compile_statement(inner);
        }
        // Verifier accommodation: both branches must enter the merge with the same stack
        // height. Source blocks are statements only, and this jump prevents then fallthrough
        // from executing else bytecode while still making every emitted pc reachable.
        const auto jump_to_end = emit(OpCode::Jump, -1);
        const auto else_pc = pc();
        patch(jump_to_else, else_pc);
        for (const auto& inner : statement.else_branch) {
            compile_statement(inner);
        }
        patch(jump_to_end, pc());
    }

    void compile_while(const Statement& statement) {
        const auto header = pc();
        compile_expr(*statement.condition);
        const auto jump_to_exit = emit(OpCode::JumpIfFalse, -1);
        for (const auto& inner : statement.body) {
            compile_statement(inner);
        }
        // Verifier accommodation: loop bodies are stack-neutral and locals keep their
        // declared source type, matching the verifier's strict merge at the backedge.
        emit(OpCode::Jump, static_cast<std::int64_t>(header));
        patch(jump_to_exit, pc());
    }

    void compile_expr(const Expr& expression) {
        switch (expression.kind) {
        case Expr::Kind::IntLiteral:
            emit(OpCode::ConstantI64, expression.int_value);
            break;
        case Expr::Kind::BoolLiteral:
            compile_bool_literal(expression.bool_value);
            break;
        case Expr::Kind::NilLiteral:
            emit(OpCode::Nil, 0);
            break;
        case Expr::Kind::Variable:
            emit(OpCode::LoadLocal, expression.local_index);
            break;
        case Expr::Kind::PairLiteral:
            compile_expr(*expression.left);
            compile_expr(*expression.right);
            emit(OpCode::AllocPair, 0);
            break;
        case Expr::Kind::Binary:
            compile_expr(*expression.left);
            compile_expr(*expression.right);
            emit(expression.binary_op == '+' ? OpCode::AddI64 : OpCode::LessI64, 0);
            break;
        case Expr::Kind::Field:
            compile_expr(*expression.receiver);
            emit(expression.name == "left" ? OpCode::GetLeft : OpCode::GetRight, 0);
            break;
        case Expr::Kind::Call:
            for (const auto& argument : expression.arguments) {
                compile_expr(*argument);
            }
            emit(OpCode::Call, static_cast<std::int64_t>(expression.callee_index));
            break;
        case Expr::Kind::IsNil:
            compile_expr(*expression.receiver);
            emit(OpCode::IsNil, 0);
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

    void compile_lvalue_receiver(const LValue& lvalue) {
        emit(OpCode::LoadLocal, lvalue.local_index);
        for (std::size_t i = 0; i + 1 < lvalue.fields.size(); ++i) {
            emit(lvalue.fields[i].name == "left" ? OpCode::GetLeft : OpCode::GetRight, 0);
        }
    }

    Function function_;
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
    module.functions.reserve(program.functions.size() + 1);

    FunctionSignature entry_signature;
    entry_signature.return_type = bytecode_kind(result_type);
    entry_signature.return_type_detail = signature_value_from_type(result_type);
    Compiler entry_compiler(program.entry_local_count, std::move(entry_signature));
    module.functions.push_back(entry_compiler.compile(program.statements, *program.result));

    for (const auto& declaration : program.functions) {
        Compiler function_compiler(declaration.local_count,
                                   signature_from_types(declaration.parameters,
                                                        declaration.return_type));
        module.functions.push_back(
            function_compiler.compile(declaration.statements, *declaration.result));
    }

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
