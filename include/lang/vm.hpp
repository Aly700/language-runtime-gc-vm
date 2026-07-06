#pragma once

#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/value.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lang {

struct VMMetrics {
    std::uint64_t instructions_executed{0};
    gc::HeapMetrics heap;
};

class VM : public gc::RootProvider {
public:
    VM();
    VM(const VM&) = delete;
    VM& operator=(const VM&) = delete;

    Value execute(const Function& function);
    Value execute(const Module& module);
    void set_max_call_depth(std::size_t max_call_depth) { max_call_depth_ = max_call_depth; }
    void set_gc_stress(gc::StressConfig config);
    [[nodiscard]] gc::StressConfig gc_stress_config() const { return gc_stress_; }
    void trace_roots(gc::RootVisitor& visitor) override;

    [[nodiscard]] const gc::Heap& heap() const { return heap_; }
    [[nodiscard]] gc::Heap& heap() { return heap_; }
    [[nodiscard]] VMMetrics metrics() const {
        return VMMetrics{instructions_executed_, heap_.metrics()};
    }

private:
    struct Frame {
        std::size_t function_index{0};
        std::size_t pc{0};
        std::vector<Value> stack;
        std::vector<Value> locals;
    };

    void collect_at_instruction_boundary_if_needed(const ModuleVerificationResult& verification,
                                                   const Frame& frame);
    void assert_stack_matches_map(const ModuleVerificationResult& verification,
                                  const Frame& frame) const;
    Value pop(Frame& frame);
    void push(Frame& frame, Value value);
    void push_frame(const Module& module, std::size_t function_index,
                    std::vector<Value> arguments);
    std::vector<Frame> frames_;
    gc::Heap heap_;
    gc::StressConfig gc_stress_{};
    std::uint64_t instructions_executed_{0};
    std::size_t max_call_depth_{1024};
};

} // namespace lang
