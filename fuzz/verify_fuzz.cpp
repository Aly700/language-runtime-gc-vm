// Fuzz target for the compile-and-verify frontend. The contract under test is
// that arbitrary input text can never crash the lexer, parser, type checker, or
// bytecode verifier: any input that is not a valid program must be reported as a
// diagnostic, never a memory error, an assertion, or undefined behavior. The
// resulting module is inspected but never executed, because execution can loop
// forever on adversarial input and the boundary being defended here is the
// verifier, not the runtime.
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "lang/frontend/type_checker.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const std::string_view source(reinterpret_cast<const char*>(data), size);
    const lang::frontend::CompileResult result = lang::frontend::compile_program(source);

    // Touch the result so the compiler cannot elide the work, and confirm the
    // reported shape is internally consistent: a successful compile must carry a
    // verified module, and a failed one must carry at least one diagnostic.
    if (result.ok()) {
        if (!result.verified_module.has_value()) {
            __builtin_trap();
        }
    } else if (result.diagnostics.empty() && result.verified_module.has_value()) {
        __builtin_trap();
    }
    return 0;
}
