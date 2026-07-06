#include "lang/gc/heap.hpp"

#include <stdexcept>

namespace lang::gc {

ObjectId Heap::allocate_pair(Value left, Value right) {
    for (ObjectId i = 0; i < objects_.size(); ++i) {
        if (!objects_[i].has_value()) {
            objects_[i] = Object{false, left, right};
            return i;
        }
    }
    objects_.push_back(Object{false, left, right});
    return static_cast<ObjectId>(objects_.size() - 1);
}

const Object& Heap::object(ObjectId id) const {
    if (id >= objects_.size() || !objects_[id].has_value()) {
        throw std::out_of_range("invalid object id");
    }
    return *objects_[id];
}

Object& Heap::object(ObjectId id) {
    if (id >= objects_.size() || !objects_[id].has_value()) {
        throw std::out_of_range("invalid object id");
    }
    return *objects_[id];
}

void Heap::mark_value(Value value) {
    if (value.is_object()) {
        mark_object(value.as_object());
    }
}

void Heap::mark_object(ObjectId id) {
    if (id >= objects_.size() || !objects_[id].has_value()) {
        return;
    }
    auto& obj = *objects_[id];
    if (obj.marked) {
        return;
    }
    obj.marked = true;
    mark_value(obj.left);
    mark_value(obj.right);
}

void Heap::collect(const std::vector<Value>& roots) {
    for (const auto& root : roots) {
        mark_value(root);
    }
    for (auto& slot : objects_) {
        if (!slot.has_value()) {
            continue;
        }
        if (slot->marked) {
            slot->marked = false;
        } else {
            slot.reset();
        }
    }
}

std::size_t Heap::live_count() const {
    std::size_t n = 0;
    for (const auto& slot : objects_) {
        if (slot.has_value()) ++n;
    }
    return n;
}

} // namespace lang::gc
