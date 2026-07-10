#pragma once

#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/value.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace lang {

struct VMMetrics {
    std::uint64_t instructions_executed{0};
    std::uint64_t raw_module_executions{0};
    std::uint64_t raw_function_executions{0};
    gc::HeapMetrics heap;
};

class VM : public gc::RootProvider {
public:
    VM();
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    // Raw bytecode entry points are compatibility paths for hand-built modules and
    // verifier-rejection tests. They always verify before dispatch and increment the
    // raw_*_executions metrics so accidental use is visible.
    Value execute(const Function& function);
    Value execute(const Module& module);
    Value execute(const VerifiedModule& module);
    void set_max_call_depth(std::size_t max_call_depth) { max_call_depth_ = max_call_depth; }
    void set_gc_stress(gc::StressConfig config);
    [[nodiscard]] gc::StressConfig gc_stress_config() const { return gc_stress_; }
    void trace_roots(gc::RootVisitor& visitor) override;

    [[nodiscard]] const gc::Heap& heap() const { return heap_; }
    [[nodiscard]] gc::Heap& heap() { return heap_; }
    [[nodiscard]] VMMetrics metrics() const {
        return VMMetrics{instructions_executed_, raw_module_executions_,
                         raw_function_executions_, heap_.metrics()};
    }

private:
    struct Frame {
        std::size_t function_index{0};
        std::size_t pc{0};
        std::vector<Value> stack;
        std::vector<Value> locals;
        std::optional<Value> closure;
    };

    void collect_at_instruction_boundary_if_needed(const ModuleVerificationResult& verification,
                                                   const Frame& frame);
    void assert_stack_matches_map(const ModuleVerificationResult& verification,
                                  const Frame& frame) const;
    Value execute_unverified_module(const Module& module);
    Value execute_verified(const Module& module,
                           const ModuleVerificationResult& verification);
    Value pop(Frame& frame);
    void push(Frame& frame, Value value);
    void push_frame(const Module& module, std::size_t function_index,
                    std::vector<Value> arguments,
                    std::optional<Value> closure = std::nullopt);
    std::vector<Frame> frames_;
    gc::Heap heap_;
    gc::StressConfig gc_stress_{};
    std::uint64_t instructions_executed_{0};
    std::uint64_t raw_module_executions_{0};
    std::uint64_t raw_function_executions_{0};
    std::size_t max_call_depth_{1024};
};

} // namespace lang
