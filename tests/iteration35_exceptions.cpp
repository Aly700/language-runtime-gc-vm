#include "lang/frontend/type_checker.hpp"
#include "lang/vm.hpp"

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}

void typed_throw_catch_executes() {
    const std::string source = R"(
variant Error { Bad(i64), Other(str) }
fn fail() -> i64 { throw Error.Bad(41); 0 }
let answer: i64 = 0;
try {
  answer = fail();
  print(to_str(answer));
} catch (error: Error) {
  match error { Bad(code) => { print(to_str(code + 1)); }, Other(text) => { print(text); } }
}
let result: i64 = 7;
result
)";
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(), compiled.diagnostics.empty() ? "compile failed" : compiled.diagnostics[0].message);
    lang::VM vm;
    const auto result = vm.execute(*compiled.verified_module);
    require(result.tag() == lang::Value::Tag::Int64 && result.as_i64() == 7, "catch changed final result");
    require(std::string(vm.output().begin(), vm.output().end()) == "42\n", "wrong catch output");
}

void traps_are_not_catchable() {
    const std::string source = R"(
variant Error { Bad() }
let value: i64 = 0;
try { value = to_i64("bad"); } catch (error: Error) { print("caught"); }
let result: i64 = 1;
result
)";
    auto compiled = lang::frontend::compile_program(source);
    require(compiled.ok(), compiled.diagnostics.empty() ? "trap separation source did not compile" : compiled.diagnostics[0].message);
    lang::VM vm;
    try { (void)vm.execute(*compiled.verified_module); }
    catch (const std::exception& error) {
        require(std::string(error.what()).find("invalid string for i64 conversion") != std::string::npos,
                "runtime trap was translated or hidden");
        return;
    }
    throw std::runtime_error("runtime trap was caught by language catch");
}
}

int main() {
    try {
        typed_throw_catch_executes();
        std::cout << "[PASS] typed_throw_catch_executes\n";
        traps_are_not_catchable();
        std::cout << "[PASS] traps_are_not_catchable\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << "\n";
        return 1;
    }
}
