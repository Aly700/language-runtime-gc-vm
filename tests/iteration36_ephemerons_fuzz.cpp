#include "fuzz_common.hpp"
#include "lang/frontend/type_checker.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr std::uint64_t kSeeds = 32;

std::string source_for(std::uint64_t seed) {
    fuzz::SplitMix64 random(seed ^ 0xE9E4'36A5ull);
    const auto a = random.small_i64() + 50;
    const auto b = random.small_i64() + 50;
    const auto c = random.small_i64() + 50;
    std::ostringstream out;
    out << "let key: pair<i64, i64> = pair(" << a << ", " << b << ");\n"
        << "let first: pair<i64, i64> = pair(" << (a + b) << ", " << seed << ");\n"
        << "let entry: ephemeron<pair<i64, i64>, pair<i64, i64>> = ephemeron(key, first);\n"
        << "let replacement: pair<i64, i64> = pair(" << c << ", " << (c + 1) << ");\n"
        << "entry.set_value(replacement);\n"
        << "let got: pair<i64, i64> = entry.value();\n"
        << "let answer: i64 = 0;\n"
        << "if is_nil(got) { answer = -1; } else { answer = got.left + key.right; }\n"
        << "print(to_str(answer));\nanswer\n";
    return out.str();
}

const std::vector<std::pair<std::string, std::string>>& mutants() {
    static const std::vector<std::pair<std::string, std::string>> values{
        {"scalar_key", "let e: ephemeron<i64, i64> = ephemeron(1, 2); 0\n"},
        {"nil_key", "let e: ephemeron<pair<i64, i64>, i64> = ephemeron(nil, 2); 0\n"},
        {"wrong_arity", "let p: pair<i64, i64> = pair(1, 2); let e: ephemeron<pair<i64, i64>, i64> = ephemeron(p); 0\n"},
        {"bad_receiver", "let p: pair<i64, i64> = pair(1, 2); p.value()\n"},
        {"setter_mismatch", "let p: pair<i64, i64> = pair(1, 2); let e: ephemeron<pair<i64, i64>, i64> = ephemeron(p, 1); e.set_value(true); 0\n"},
        {"unguarded_value", "let p: pair<i64, i64> = pair(1, 2); let v: pair<i64, i64> = pair(3, 4); let e: ephemeron<pair<i64, i64>, pair<i64, i64>> = ephemeron(p, v); let got: pair<i64, i64> = e.value(); got.left\n"},
    };
    return values;
}

lang::VerifiedModule compile_seed(std::uint64_t seed) {
    auto result = lang::frontend::compile_program(source_for(seed));
    if (!result.ok()) {
        throw std::runtime_error("ephemerons source rejected seed=" +
                                 std::to_string(seed) + ": " +
                                 (result.diagnostics.empty()
                                      ? std::string("no diagnostic")
                                      : result.diagnostics.front().message));
    }
    return *result.verified_module;
}

void require_mutants_rejected() {
    for (const auto& [name, source] : mutants()) {
        const auto result = lang::frontend::compile_program(source);
        if (result.ok() || result.diagnostics.empty())
            throw std::runtime_error("ephemeron mutant accepted: " + name);
    }
}
}

int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--dump-corpus" &&
            std::string(argv[2]) == "ephemerons") {
            for (std::uint64_t seed = 1; seed <= kSeeds; ++seed)
                std::cout << "===== seed " << seed << " =====\n" << source_for(seed);
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--mutant") {
            for (const auto& [name, source] : mutants()) {
                if (name == argv[2]) {
                    const auto result = lang::frontend::compile_program(source);
                    return result.ok() ? 1 : 0;
                }
            }
            throw std::runtime_error("unknown mutant");
        }
        const auto schedules = fuzz::schedules();
        if (schedules.size() != 15) throw std::runtime_error("expected fifteen schedules");
        if (argc == 5 && std::string(argv[1]) == "--seed" &&
            std::string(argv[3]) == "--schedule") {
            const auto seed = static_cast<std::uint64_t>(std::stoull(argv[2]));
            const auto schedule = static_cast<std::size_t>(std::stoull(argv[4]));
            if (seed == 0 || seed > kSeeds || schedule >= schedules.size())
                throw std::runtime_error("replay argument out of range");
            const auto result = fuzz::execute_once(compile_seed(seed), schedules[schedule]);
            if (!result.ok) throw std::runtime_error(result.error);
            std::cout << result.observable << '\n' << result.output;
            return 0;
        }
        require_mutants_rejected();
        for (std::uint64_t seed = 1; seed <= kSeeds; ++seed) {
            const auto module = compile_seed(seed);
            const auto baseline = fuzz::execute_once(module, schedules.front());
            if (!baseline.ok) throw std::runtime_error(baseline.error);
            for (const auto& schedule : schedules) {
                const auto observed = fuzz::execute_once(module, schedule);
                if (!observed.ok || !fuzz::same_observables(baseline, observed))
                    throw std::runtime_error("ephemeron mismatch seed=" +
                                             std::to_string(seed) +
                                             " schedule=" + schedule.name);
            }
        }
        std::cerr << "[PASS] lang_iteration36_ephemerons_fuzz seeds=32 schedules=15 executions=480 mutants=6\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "[FAIL] " << error.what() << '\n';
        return 1;
    }
}
