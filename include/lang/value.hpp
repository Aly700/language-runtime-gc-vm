#pragma once

#include <cstdint>
#include <limits>
#include <stdexcept>

namespace lang {

using ObjectId = std::uint64_t;

class Value {
public:
    enum class Tag { Int64, Bool, Object, Nil };

    static Value int64(std::int64_t v) { return Value(Tag::Int64, v); }
    static Value boolean(bool v) { return Value(Tag::Bool, v ? 1 : 0); }
    static Value object(ObjectId id) {
        if (id > static_cast<ObjectId>(std::numeric_limits<std::int64_t>::max())) {
            throw std::out_of_range("object id does not fit in Value payload");
        }
        return Value(Tag::Object, static_cast<std::int64_t>(id));
    }
    static Value nil() { return Value(Tag::Nil, 0); }

    [[nodiscard]] Tag tag() const { return tag_; }
    [[nodiscard]] bool is_object() const { return tag_ == Tag::Object; }

    [[nodiscard]] std::int64_t as_i64() const {
        if (tag_ != Tag::Int64) throw std::logic_error("value is not i64");
        return payload_;
    }

    [[nodiscard]] bool as_bool() const {
        if (tag_ != Tag::Bool) throw std::logic_error("value is not bool");
        return payload_ != 0;
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
