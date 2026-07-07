#include "lang/bytecode.hpp"
#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"

#include <cstdint>
#include <exception>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void set_signature(lang::Function& function,
                   std::initializer_list<lang::ValueKind> parameters,
                   lang::ValueKind result) {
    function.signature.parameters.assign(parameters.begin(), parameters.end());
    function.signature.return_type = result;
}

lang::Module valid_i64_module(std::int64_t value) {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);
    auto& entry = module.functions[0];
    set_signature(entry, {}, lang::ValueKind::Int64);
    entry.code = {
        {lang::OpCode::ConstantI64, value},
        {lang::OpCode::Return, 0},
    };
    return module;
}

lang::Module stack_underflow_module() {
    lang::Module module;
    module.entry_function = 0;
    module.functions.resize(1);
    auto& entry = module.functions[0];
    set_signature(entry, {}, lang::ValueKind::Int64);
    entry.code = {
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };
    return module;
}

std::string source_listing(const std::string& source) {
    std::ostringstream out;
    out << "source:\n" << source << "\n";
    return out.str();
}

void raw_invalid_module_still_rejects_at_execute() {
    auto module = stack_underflow_module();
    lang::VM vm;
    try {
        (void)vm.execute(module);
    } catch (const std::exception& e) {
        const std::string message = e.what();
        require(message.find("bytecode verifier rejected module") != std::string::npos,
                "raw execute did not identify verifier rejection: " + message);
        require(message.find("StackUnderflow") != std::string::npos,
                "raw execute did not preserve verifier diagnostic: " + message);
        return;
    }
    throw std::runtime_error("raw execute accepted invalid module");
}

void verified_module_executes_through_verified_overload() {
    auto verified = lang::verify_module(valid_i64_module(42));
    require(verified.has_value(), "verify_module rejected a valid module");

    lang::VM vm;
    const auto result = vm.execute(*verified);
    require(result.as_i64() == 42, "verified module returned wrong value");
    const auto metrics = vm.metrics();
    require(metrics.raw_module_executions == 0,
            "verified module execution should not enter the raw Module path");
    require(metrics.raw_function_executions == 0,
            "verified module execution should not enter the raw Function path");
}

void verified_module_owns_immutable_copy_after_verification() {
    auto module = valid_i64_module(17);
    auto verified = lang::verify_module(module);
    require(verified.has_value(), "verify_module rejected a valid module copy");

    module = stack_underflow_module();
    lang::VM raw_vm;
    try {
        (void)raw_vm.execute(module);
    } catch (const std::exception&) {
        lang::VM verified_vm;
        const auto result = verified_vm.execute(*verified);
        require(result.as_i64() == 17,
                "verified module did not preserve its pre-mutation bytecode");
        return;
    }
    throw std::runtime_error("mutated raw module should reject after verification copy");
}

void compile_program_returns_executable_verified_module() {
    const std::string source = "let x: i64 = 40; x + 2";
    auto compiled = lang::frontend::compile_program(source);
    if (!compiled.ok()) {
        std::ostringstream out;
        out << "source failed to compile\n" << source_listing(source);
        for (const auto& diagnostic : compiled.diagnostics) {
            out << diagnostic.position.line << ":" << diagnostic.position.column << " "
                << diagnostic.message << "\n";
        }
        throw std::runtime_error(out.str());
    }
    require(compiled.verified_module.has_value(),
            "compile_program did not return a verified module");

    lang::VM vm;
    const auto result = vm.execute(*compiled.verified_module);
    require(result.as_i64() == 42,
            "compiled verified module returned wrong value\n" + source_listing(source));
    const auto metrics = vm.metrics();
    require(metrics.raw_module_executions == 0,
            "compiled verified module should not re-enter raw Module execution");
    require(metrics.raw_function_executions == 0,
            "compiled verified module should not re-enter raw Function execution");
}

void raw_module_execution_is_counted_for_review_visibility() {
    lang::VM vm;
    const auto result = vm.execute(valid_i64_module(9));
    require(result.as_i64() == 9, "raw module execution returned wrong value");

    const auto metrics = vm.metrics();
    require(metrics.raw_module_executions == 1,
            "raw Module execution should be visible in VM metrics");
    require(metrics.raw_function_executions == 0,
            "raw Module execution should not be counted as raw Function execution");
}

static_assert(!std::is_constructible_v<lang::VerifiedModule, lang::Module>,
              "VerifiedModule must not be constructible directly from raw Module");
static_assert(!std::is_constructible_v<lang::VerifiedModule, const lang::Module&>,
              "VerifiedModule must not wrap a mutable external Module by reference");
static_assert(std::is_same_v<decltype(std::declval<const lang::VerifiedModule&>().module()),
                             const lang::Module&>,
              "VerifiedModule must expose only const module access");

template <typename Result>
concept HasRawCompileResultModule = requires(Result result) {
    result.module;
};

template <typename Result>
concept HasRawCompileResultFunction = requires(Result result) {
    result.function;
};

static_assert(std::is_same_v<decltype((std::declval<lang::frontend::CompileResult&>()
                                           .verified_module)),
                             std::optional<lang::VerifiedModule>&>,
              "compile_program should expose a single verified module product");
static_assert(!HasRawCompileResultModule<lang::frontend::CompileResult>,
              "CompileResult must not expose a parallel raw Module");
static_assert(!HasRawCompileResultFunction<lang::frontend::CompileResult>,
              "CompileResult must not expose a parallel raw Function");

struct TestCase {
    const char* name;
    const char* proves;
    const char* baseline_red;
    void (*run)();
};

} // namespace

int main() {
    const std::vector<TestCase> tests = {
        {"raw_invalid_module_still_rejects_at_execute",
         "the raw Module execution path still runs the verifier and preserves diagnostics",
         "BASELINE-GREEN: existing VM::execute(Module) already rejects this invalid module",
         raw_invalid_module_still_rejects_at_execute},
        {"verified_module_executes_through_verified_overload",
         "VM::execute accepts a verifier-produced immutable module proof",
         "BASELINE-RED on b793363: VerifiedModule and verify_module do not exist",
         verified_module_executes_through_verified_overload},
        {"verified_module_owns_immutable_copy_after_verification",
         "post-verification mutation of the original Module cannot affect verified execution",
         "BASELINE-RED on b793363: VerifiedModule and verify_module do not exist",
         verified_module_owns_immutable_copy_after_verification},
        {"compile_program_returns_executable_verified_module",
         "compile_program carries its existing verifier proof to the VM without raw re-entry",
         "BASELINE-RED on b793363: CompileResult has no verified_module field",
         compile_program_returns_executable_verified_module},
        {"raw_module_execution_is_counted_for_review_visibility",
         "deliberate raw Module execution remains possible but visible in VM metrics",
         "BASELINE-RED: raw Module execution metrics do not exist",
         raw_module_execution_is_counted_for_review_visibility},
    };

    for (const auto& test : tests) {
        try {
            test.run();
        } catch (const std::exception& e) {
            std::cerr << "[FAIL] " << test.name << "\n"
                      << "  proves: " << test.proves << "\n"
                      << "  baseline: " << test.baseline_red << "\n"
                      << "  error: " << e.what() << "\n";
            return 1;
        }
    }

    std::cerr << "[PASS] lang_iteration17_verified_module tests=" << tests.size()
              << "\n";
    return 0;
}
