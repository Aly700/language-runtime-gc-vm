#pragma once

#include "lang/bytecode.hpp"
#include "lang/gc/heap.hpp"
#include "lang/value.hpp"

#include <vector>

namespace lang {

class VM {
public:
    Value execute(const Function& function);

    [[nodiscard]] const gc::Heap& heap() const { return heap_; }
    [[nodiscard]] gc::Heap& heap() { return heap_; }

private:
    Value pop();
    void push(Value value);
    std::vector<Value> stack_;
    std::vector<Value> locals_;
    gc::Heap heap_;
};

} // namespace lang
