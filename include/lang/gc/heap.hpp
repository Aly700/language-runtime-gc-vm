#pragma once

#include "lang/value.hpp"

#include <optional>
#include <vector>

namespace lang::gc {

struct Object {
    bool marked{false};
    Value left{Value::nil()};
    Value right{Value::nil()};
};

class Heap {
public:
    ObjectId allocate_pair(Value left, Value right);
    void collect(const std::vector<Value>& roots);

    [[nodiscard]] const Object& object(ObjectId id) const;
    [[nodiscard]] Object& object(ObjectId id);
    [[nodiscard]] std::size_t live_count() const;
    [[nodiscard]] std::size_t capacity_slots() const { return objects_.size(); }

private:
    void mark_value(Value value);
    void mark_object(ObjectId id);

    std::vector<std::optional<Object>> objects_;
};

} // namespace lang::gc
