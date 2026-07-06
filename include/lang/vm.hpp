#pragma once

#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/value.hpp"

#include <cstdint>
#include <vector>

namespace lang {

class VM : public gc::RootProvider {
public:
    VM();
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    Value execute(const Function& function);
    void set_gc_stress(gc::StressConfig config);
    [[nodiscard]] gc::StressConfig gc_stress_config() const { return gc_stress_; }
    void trace_roots(gc::RootVisitor& visitor) override;

    [[nodiscard]] const gc::Heap& heap() const { return heap_; }
    [[nodiscard]] gc::Heap& heap() { return heap_; }

private:
    void collect_at_instruction_boundary_if_needed(const VerificationResult& verification,
                                                   std::size_t pc);
    void assert_stack_matches_map(const VerificationResult& verification, std::size_t pc) const;
    Value pop();
    void push(Value value);
    std::vector<Value> stack_;
    std::vector<Value> locals_;
    gc::Heap heap_;
    gc::StressConfig gc_stress_{};
    std::uint64_t instructions_executed_{0};
};

} // namespace lang
