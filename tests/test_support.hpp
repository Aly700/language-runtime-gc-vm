#pragma once

#include "lang/bytecode.hpp"
#include "lang/value.hpp"
#include "lang/vm.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace test_support {

inline lang::Module module_from_function(const lang::Function& function) {
    lang::Module module;
    module.entry_function = 0;
    module.functions.push_back(function);
    return module;
}

inline lang::VerifiedModule verify_module_or_throw(lang::Module module,
                                                   const std::string& context) {
    auto verified = lang::verify_module(std::move(module));
    if (!verified.has_value()) {
        throw std::runtime_error("test module failed bytecode verification\n" +
                                 context);
    }
    return std::move(*verified);
}

inline lang::VerifiedModule verify_function_or_throw(const lang::Function& function,
                                                     const std::string& context) {
    return verify_module_or_throw(module_from_function(function), context);
}

inline lang::Value execute_verified(lang::VM& vm, const lang::Function& function,
                                    const std::string& context) {
    auto verified = verify_function_or_throw(function, context);
    return vm.execute(verified);
}

inline lang::Value execute_verified(lang::VM& vm, lang::Module module,
                                    const std::string& context) {
    auto verified = verify_module_or_throw(std::move(module), context);
    return vm.execute(verified);
}

} // namespace test_support
