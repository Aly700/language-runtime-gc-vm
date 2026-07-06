#pragma once

#include <cstdint>
#include <stdexcept>

namespace lang {

using ObjectId = std::uint32_t;

class Value {
public:
    enum class Tag { Int64, Bool, Object, Nil };

    static Value int64(std::int64_t v) { return Value(Tag::Int64, v); }
    static Value boolean(bool v) { return Value(Tag::Bool, v ? 1 : 0); }
    static Value object(ObjectId id) { return Value(Tag::Object, id); }
    static Value nil() { return Value(Tag::Nil, 0); }

    [[nodiscard]] Tag tag() const { return tag_; }
    [[nodiscard]] bool is_object() const { return tag_ == Tag::Object; }

    [[nodiscard]] std::int64_t as_i64() const {
        if (tag_ != Tag::Int64) throw std::logic_error("value is not i64");
        return payload_;
    }

    [[nodiscard]] ObjectId as_object() const {
        if (tag_ != Tag::Object) throw std::logic_error("value is not object");
        return static_cast<ObjectId>(payload_);
    }

private:
    Value(Tag tag, std::int64_t payload) : tag_(tag), payload_(payload) {}

    Tag tag_{Tag::Nil};
    std::int64_t payload_{0};
};

} // namespace lang
