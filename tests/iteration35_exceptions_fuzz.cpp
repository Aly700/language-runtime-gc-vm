#include "fuzz_common.hpp"
#include "lang/frontend/type_checker.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace {
constexpr std::uint64_t kSeeds = 32;

std::string source_for(std::uint64_t seed) {
    std::ostringstream out;
    out << "variant Error { Code(i64, pair<i64, i64>), Text(str) }\n"
        << "fn fail() -> i64 { throw Error.Code(" << (seed * 17 + 3)
        << ", pair(" << seed << ", " << (seed + 1) << ")); 0 }\n"
        << "let answer: i64 = 0;\n"
        << "try { answer = fail(); } catch (error: Error) {\n"
        << "  match error { Code(code, payload) => { answer = code + payload.left; print(to_str(answer)); }, Text(text) => { print(text); } }\n"
        << "}\nanswer\n";
    return out.str();
}

lang::VerifiedModule compile(std::uint64_t seed) {
    auto result = lang::frontend::compile_program(source_for(seed));
    if (!result.ok()) throw std::runtime_error("exceptions source rejected seed=" + std::to_string(seed));
    return *result.verified_module;
}
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--dump-corpus" &&
        std::string(argv[2]) == "exceptions") {
        for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
            std::cout << "===== seed " << seed << " =====\n" << source_for(seed);
        }
        return 0;
    }
    const auto schedules = fuzz::schedules();
    if (schedules.size() != 15) throw std::runtime_error("expected fifteen schedules");
    for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
        const auto module = compile(seed);
        const auto baseline = fuzz::execute_once(module, schedules.front());
        if (!baseline.ok) throw std::runtime_error(baseline.error);
        for (const auto& schedule : schedules) {
            const auto observed = fuzz::execute_once(module, schedule);
            if (!observed.ok || !fuzz::same_observables(baseline, observed)) {
                throw std::runtime_error("exception determinism mismatch seed=" +
                                         std::to_string(seed) + " schedule=" + schedule.name);
            }
        }
    }
    std::cerr << "[PASS] lang_iteration35_exceptions_fuzz seeds=32 schedules=15 executions=480\n";
}
