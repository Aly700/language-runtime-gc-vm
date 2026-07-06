#include "lang/gc/heap.hpp"
#include "lang/vm.hpp"

#include <cassert>

int main() {
    lang::Function add;
    add.code = {
        {lang::OpCode::ConstantI64, 40},
        {lang::OpCode::ConstantI64, 2},
        {lang::OpCode::AddI64, 0},
        {lang::OpCode::Return, 0},
    };

    lang::VM vm;
    auto value = vm.execute(add);
    assert(value.as_i64() == 42);

    lang::gc::Heap heap;
    auto live = heap.allocate_pair(lang::Value::int64(1), lang::Value::int64(2));
    heap.allocate_pair(lang::Value::int64(3), lang::Value::int64(4));
    heap.collect({lang::Value::object(live)});
    assert(heap.live_count() == 1);
    assert(heap.object(live).left.as_i64() == 1);

    return 0;
}
