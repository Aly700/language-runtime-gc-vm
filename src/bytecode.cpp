#include "lang/bytecode.hpp"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lang {

namespace {

enum class AbstractKind {
    Int64,
    Bool,
    Object,
    Array,
    RefArray,
    Str,
    Function,
    Map,
    Weak,
    MaybeReference,
    Record,
    Variant,
    Nil,
    Poison,
};

struct PairFields;

struct AbstractValue {
    AbstractKind kind{AbstractKind::Poison};
    std::set<std::size_t> object_sites;
    std::set<std::size_t> ref_array_sites;
    bool includes_opaque_object{false};
    bool includes_nil{false};
    std::shared_ptr<const PairFields> signature_fields;
    std::shared_ptr<const AbstractValue> signature_array_element;
    std::shared_ptr<const SignatureValue> signature_function;
    std::shared_ptr<const SignatureValue> signature_map;
    std::shared_ptr<const AbstractValue> reference_target;
    std::optional<std::size_t> signature_named_type;
    std::optional<std::size_t> signature_record_layout;
    std::optional<std::size_t> signature_variant_layout;
    std::optional<std::size_t> source_local;
    std::optional<std::size_t> nil_test_local;
};

struct PairFields {
    AbstractValue left;
    AbstractValue right;
};

struct AbstractState {
    std::vector<AbstractValue> stack;
    std::vector<std::optional<AbstractValue>> locals;
    // Root category is tracked independently from definite initialization. At a loop
    // header an unread local may contain Nil on the entry edge and a reference on the
    // backedge; the local still needs a precise reference bit while LoadLocal remains
    // forbidden until every incoming edge initializes it.
    std::vector<std::optional<bool>> local_reference_kinds;
    std::map<std::size_t, PairFields> fields_by_site;
    std::map<std::size_t, AbstractValue> ref_array_elements_by_site;
};

enum class JoinOutcome {
    Unchanged,
    Changed,
    Invalid,
};

const char* op_name(OpCode op) {
    switch (op) {
    case OpCode::ConstantI64:
        return "ConstantI64";
    case OpCode::AddI64:
        return "AddI64";
    case OpCode::LessI64:
        return "LessI64";
    case OpCode::AllocPair:
        return "AllocPair";
    case OpCode::GetLeft:
        return "GetLeft";
    case OpCode::GetRight:
        return "GetRight";
    case OpCode::SetLeft:
        return "SetLeft";
    case OpCode::SetRight:
        return "SetRight";
    case OpCode::AllocArray:
        return "AllocArray";
    case OpCode::ArrayGet:
        return "ArrayGet";
    case OpCode::ArraySet:
        return "ArraySet";
    case OpCode::ArrayLen:
        return "ArrayLen";
    case OpCode::LoadLocal:
        return "LoadLocal";
    case OpCode::StoreLocal:
        return "StoreLocal";
    case OpCode::Jump:
        return "Jump";
    case OpCode::JumpIfFalse:
        return "JumpIfFalse";
    case OpCode::Collect:
        return "Collect";
    case OpCode::Call:
        return "Call";
    case OpCode::Return:
        return "Return";
    case OpCode::Nil:
        return "Nil";
    case OpCode::IsNil:
        return "IsNil";
    case OpCode::AllocRefArray:
        return "AllocRefArray";
    case OpCode::RefArrayGet:
        return "RefArrayGet";
    case OpCode::RefArraySet:
        return "RefArraySet";
    case OpCode::PushStr:
        return "PushStr";
    case OpCode::StrLen:
        return "StrLen";
    case OpCode::StrEq:
        return "StrEq";
    case OpCode::StrConcat:
        return "StrConcat";
    case OpCode::StrIndex:
        return "StrIndex";
    case OpCode::AllocClosure:
        return "AllocClosure";
    case OpCode::CallClosure:
        return "CallClosure";
    case OpCode::LoadCapture:
        return "LoadCapture";
    case OpCode::AllocMap:
        return "AllocMap";
    case OpCode::MapSet:
        return "MapSet";
    case OpCode::MapGet:
        return "MapGet";
    case OpCode::MapHas:
        return "MapHas";
    case OpCode::MapLen:
        return "MapLen";
    case OpCode::AllocWeak:
        return "AllocWeak";
    case OpCode::WeakGet:
        return "WeakGet";
    case OpCode::MapKeyAt:
        return "MapKeyAt";
    case OpCode::MapValueAt:
        return "MapValueAt";
    case OpCode::Print:
        return "Print";
    case OpCode::I64ToStr:
        return "I64ToStr";
    case OpCode::StrToI64:
        return "StrToI64";
    case OpCode::BoolToStr:
        return "BoolToStr";
    case OpCode::StrSub:
        return "StrSub";
    case OpCode::StrLt:
        return "StrLt";
    case OpCode::AllocRecord:
        return "AllocRecord";
    case OpCode::RecordGet:
        return "RecordGet";
    case OpCode::RecordSet:
        return "RecordSet";
    case OpCode::AllocVariant:
        return "AllocVariant";
    case OpCode::VariantTag:
        return "VariantTag";
    case OpCode::VariantGet:
        return "VariantGet";
    case OpCode::TryBegin:
        return "TryBegin";
    case OpCode::TryEnd:
        return "TryEnd";
    case OpCode::Throw:
        return "Throw";
    }
    return "<invalid>";
}

const char* value_kind_name(ValueKind kind) {
    switch (kind) {
    case ValueKind::Int64:
        return "Int64";
    case ValueKind::Bool:
        return "Bool";
    case ValueKind::Object:
        return "Object";
    case ValueKind::Array:
        return "Array";
    case ValueKind::Nil:
        return "Nil";
    case ValueKind::Str:
        return "Str";
    case ValueKind::Function:
        return "Function";
    case ValueKind::Map:
        return "Map";
    case ValueKind::Weak:
        return "Weak";
    case ValueKind::Record:
        return "Record";
    case ValueKind::Variant:
        return "Variant";
    }
    return "<invalid>";
}

const char* abstract_kind_name(AbstractKind kind) {
    switch (kind) {
    case AbstractKind::Int64:
        return "Int64";
    case AbstractKind::Bool:
        return "Bool";
    case AbstractKind::Object:
        return "Object";
    case AbstractKind::Array:
        return "Array";
    case AbstractKind::RefArray:
        return "RefArray";
    case AbstractKind::Str:
        return "Str";
    case AbstractKind::Function:
        return "Function";
    case AbstractKind::Map:
        return "Map";
    case AbstractKind::Weak:
        return "Weak";
    case AbstractKind::MaybeReference:
        return "MaybeReference";
    case AbstractKind::Record:
        return "Record";
    case AbstractKind::Variant:
        return "Variant";
    case AbstractKind::Nil:
        return "Nil";
    case AbstractKind::Poison:
        return "Poison";
    }
    return "<invalid>";
}

bool reject(std::vector<VerifierDiagnostic>& diagnostics,
            std::size_t function_index,
            std::optional<std::size_t> pc,
            VerifierReason reason,
            std::string message) {
    diagnostics.push_back(
        VerifierDiagnostic{function_index, pc, reason, std::move(message)});
    return false;
}

std::string instruction_message(const Function& function, std::size_t pc,
                                std::string_view detail) {
    std::ostringstream out;
    out << op_name(function.code[pc].op) << " at pc " << pc << ": " << detail;
    return out.str();
}

AbstractValue value_with_kind(AbstractKind kind) {
    AbstractValue value;
    value.kind = kind;
    return value;
}

AbstractValue int64_value() { return value_with_kind(AbstractKind::Int64); }
AbstractValue bool_value() { return value_with_kind(AbstractKind::Bool); }
AbstractValue array_value() { return value_with_kind(AbstractKind::Array); }
AbstractValue ref_array_value() { return value_with_kind(AbstractKind::RefArray); }
AbstractValue str_value() { return value_with_kind(AbstractKind::Str); }

AbstractValue map_value(SignatureValue signature) {
    AbstractValue value;
    value.kind = AbstractKind::Map;
    value.signature_map =
        std::make_shared<const SignatureValue>(std::move(signature));
    return value;
}

bool signature_values_equal(const SignatureValue& lhs,
                            const SignatureValue& rhs);

bool signature_value_ptr_equal(const std::shared_ptr<SignatureValue>& lhs,
                               const std::shared_ptr<SignatureValue>& rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    return signature_values_equal(*lhs, *rhs);
}

bool signature_values_equal(const SignatureValue& lhs,
                            const SignatureValue& rhs) {
    if (lhs.kind != rhs.kind || lhs.named_type != rhs.named_type ||
        lhs.record_layout != rhs.record_layout ||
        lhs.variant_layout != rhs.variant_layout ||
        lhs.function_parameters.size() != rhs.function_parameters.size() ||
        !signature_value_ptr_equal(lhs.left, rhs.left) ||
        !signature_value_ptr_equal(lhs.right, rhs.right) ||
        !signature_value_ptr_equal(lhs.element, rhs.element) ||
        !signature_value_ptr_equal(lhs.key, rhs.key) ||
        !signature_value_ptr_equal(lhs.value, rhs.value) ||
        !signature_value_ptr_equal(lhs.weak_target, rhs.weak_target) ||
        !signature_value_ptr_equal(lhs.function_return, rhs.function_return)) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.function_parameters.size(); ++i) {
        if (!signature_values_equal(lhs.function_parameters[i],
                                    rhs.function_parameters[i])) {
            return false;
        }
    }
    return true;
}

AbstractValue ref_array_value(std::size_t site) {
    AbstractValue value;
    value.kind = AbstractKind::RefArray;
    value.ref_array_sites.insert(site);
    return value;
}
AbstractValue poison_value() { return value_with_kind(AbstractKind::Poison); }

AbstractValue nil_object_value() {
    AbstractValue value;
    value.kind = AbstractKind::Object;
    value.includes_nil = true;
    return value;
}

AbstractValue object_value(std::size_t site) {
    AbstractValue value;
    value.kind = AbstractKind::Object;
    value.object_sites.insert(site);
    return value;
}

AbstractValue opaque_object_value() {
    AbstractValue value;
    value.kind = AbstractKind::Object;
    value.includes_opaque_object = true;
    return value;
}

AbstractValue named_signature_object_value(std::size_t index) {
    AbstractValue value;
    value.kind = AbstractKind::Object;
    value.includes_nil = true;
    value.signature_named_type = index;
    return value;
}

AbstractValue record_value(std::size_t layout_index, bool includes_nil = true) {
    AbstractValue value;
    value.kind = AbstractKind::Record;
    value.includes_nil = includes_nil;
    value.signature_record_layout = layout_index;
    return value;
}

AbstractValue variant_value(std::size_t layout_index,
                            bool includes_nil = true) {
    AbstractValue value;
    value.kind = AbstractKind::Variant;
    value.includes_nil = includes_nil;
    value.signature_variant_layout = layout_index;
    return value;
}

AbstractValue without_provenance(AbstractValue value) {
    value.source_local.reset();
    value.nil_test_local.reset();
    return value;
}

std::shared_ptr<const PairFields> fields_ptr(PairFields fields) {
    return std::make_shared<const PairFields>(std::move(fields));
}

std::shared_ptr<const AbstractValue> abstract_value_ptr(AbstractValue value) {
    return std::make_shared<const AbstractValue>(std::move(value));
}

AbstractValue weak_value(AbstractValue target) {
    AbstractValue value;
    value.kind = AbstractKind::Weak;
    // A live WeakGet result is always the non-nil target; possible clearing is
    // represented by the surrounding MaybeReference, not by the target shape.
    target.includes_nil = false;
    value.reference_target = abstract_value_ptr(without_provenance(
        std::move(target)));
    return value;
}

AbstractValue maybe_reference_value(AbstractValue target) {
    AbstractValue value;
    value.kind = AbstractKind::MaybeReference;
    value.reference_target = abstract_value_ptr(without_provenance(
        std::move(target)));
    return value;
}

AbstractKind abstract_kind(ValueKind kind) {
    switch (kind) {
    case ValueKind::Int64:
        return AbstractKind::Int64;
    case ValueKind::Bool:
        return AbstractKind::Bool;
    case ValueKind::Object:
        return AbstractKind::Object;
    case ValueKind::Array:
        return AbstractKind::Array;
    case ValueKind::Nil:
        return AbstractKind::Nil;
    case ValueKind::Str:
        return AbstractKind::Str;
    case ValueKind::Function:
        return AbstractKind::Function;
    case ValueKind::Map:
        return AbstractKind::Map;
    case ValueKind::Weak:
        return AbstractKind::Weak;
    case ValueKind::Record:
        return AbstractKind::Record;
    case ValueKind::Variant:
        return AbstractKind::Variant;
    }
    return AbstractKind::Poison;
}

AbstractValue value_from_signature(ValueKind kind) {
    if (kind == ValueKind::Object) {
        return opaque_object_value();
    }
    return value_with_kind(abstract_kind(kind));
}

bool signature_array_uses_ref_payload(const SignatureValue& signature) {
    return signature.has_array_element() &&
           signature_value_is_reference(*signature.element);
}

AbstractValue value_from_signature(const SignatureValue& signature) {
    if (signature.is_record_layout_reference()) {
        return record_value(*signature.record_layout);
    }
    if (signature.is_variant_layout_reference()) {
        return variant_value(*signature.variant_layout);
    }
    if (signature.has_weak_target()) {
        return weak_value(value_from_signature(*signature.weak_target));
    }
    if (signature.has_function_signature()) {
        AbstractValue value;
        value.kind = AbstractKind::Function;
        value.signature_function =
            std::make_shared<const SignatureValue>(signature);
        return value;
    }
    if (signature.has_map_entries()) {
        return map_value(signature);
    }
    if (signature.is_named_type_reference()) {
        return named_signature_object_value(*signature.named_type);
    }
    if (signature.has_pair_fields()) {
        AbstractValue value;
        value.kind = AbstractKind::Object;
        value.signature_fields = fields_ptr(PairFields{
            value_from_signature(*signature.left),
            value_from_signature(*signature.right),
        });
        return value;
    }
    if (signature.has_array_element()) {
        if (!signature_array_uses_ref_payload(signature)) {
            return array_value();
        }
        auto value = ref_array_value();
        auto element = value_from_signature(*signature.element);
        if (element.kind == AbstractKind::Object ||
            element.kind == AbstractKind::Record ||
            element.kind == AbstractKind::Variant) {
            element.includes_nil = false;
        }
        value.signature_array_element = abstract_value_ptr(std::move(element));
        return value;
    }
    return value_from_signature(signature.kind);
}

bool is_reference_kind(AbstractKind kind) {
    return kind == AbstractKind::Object || kind == AbstractKind::Array ||
           kind == AbstractKind::RefArray || kind == AbstractKind::Str ||
           kind == AbstractKind::Function || kind == AbstractKind::Map ||
           kind == AbstractKind::Weak || kind == AbstractKind::MaybeReference ||
           kind == AbstractKind::Record || kind == AbstractKind::Variant;
}

SignatureValue parameter_signature(const FunctionSignature& signature,
                                   std::size_t index) {
    if (signature.parameter_types.empty()) {
        return signature_value(signature.parameters[index]);
    }
    return signature.parameter_types[index];
}

SignatureValue return_signature(const FunctionSignature& signature) {
    if (signature.return_type_detail.has_value()) {
        return *signature.return_type_detail;
    }
    return signature_value(signature.return_type);
}

SignatureValue function_type_from_signature(const FunctionSignature& signature) {
    std::vector<SignatureValue> parameters;
    parameters.reserve(signature.parameters.size());
    for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
        parameters.push_back(parameter_signature(signature, i));
    }
    return function_signature(std::move(parameters), return_signature(signature));
}

bool signature_shape_matches_kind(const SignatureValue& signature, ValueKind kind,
                                  const Module& module) {
    if (signature.kind != kind) {
        return false;
    }
    if (signature.is_record_layout_reference()) {
        return *signature.record_layout < module.record_layouts.size();
    }
    if (signature.is_variant_layout_reference()) {
        return *signature.variant_layout < module.variant_layouts.size();
    }
    if (signature.is_named_type_reference()) {
        return signature.kind == ValueKind::Object &&
               *signature.named_type < module.named_types.size() &&
               signature.left == nullptr && signature.right == nullptr &&
               signature.element == nullptr && signature.key == nullptr &&
               signature.value == nullptr && signature.weak_target == nullptr &&
               signature.function_return == nullptr &&
               signature.function_parameters.empty() &&
               !signature.record_layout.has_value() &&
               !signature.variant_layout.has_value();
    }
    if (signature.has_pair_fields()) {
        return signature_shape_matches_kind(*signature.left, signature.left->kind,
                                            module) &&
               signature_shape_matches_kind(*signature.right, signature.right->kind,
                                            module) &&
               signature.element == nullptr &&
               signature.key == nullptr && signature.value == nullptr &&
               signature.weak_target == nullptr &&
               signature.function_return == nullptr &&
               signature.function_parameters.empty() &&
               !signature.record_layout.has_value() &&
               !signature.variant_layout.has_value();
    }
    if (signature.has_array_element()) {
        return signature.kind == ValueKind::Array &&
               signature_shape_matches_kind(*signature.element,
                                            signature.element->kind, module) &&
               signature.left == nullptr && signature.right == nullptr &&
               signature.key == nullptr && signature.value == nullptr &&
               signature.weak_target == nullptr &&
               signature.function_return == nullptr &&
               signature.function_parameters.empty() &&
               !signature.record_layout.has_value() &&
               !signature.variant_layout.has_value();
    }
    if (signature.has_function_signature()) {
        if (!signature_shape_matches_kind(*signature.function_return,
                                          signature.function_return->kind,
                                          module)) {
            return false;
        }
        for (const auto& parameter : signature.function_parameters) {
            if (!signature_shape_matches_kind(parameter, parameter.kind, module)) {
                return false;
            }
        }
        return true;
    }
    if (signature.has_map_entries()) {
        return signature_shape_matches_kind(*signature.key,
                                            signature.key->kind, module) &&
               signature_shape_matches_kind(*signature.value,
                                            signature.value->kind, module);
    }
    if (signature.has_weak_target()) {
        return signature_value_is_reference(*signature.weak_target) &&
               signature_shape_matches_kind(*signature.weak_target,
                                            signature.weak_target->kind,
                                            module);
    }
    if (signature.kind == ValueKind::Function) {
        return false;
    }
    if (signature.kind == ValueKind::Map) {
        return false;
    }
    if (signature.kind == ValueKind::Weak) {
        return false;
    }
    if (signature.kind == ValueKind::Record) {
        return false;
    }
    if (signature.kind == ValueKind::Variant) {
        return false;
    }
    return signature.left == nullptr && signature.right == nullptr &&
           signature.element == nullptr && signature.function_return == nullptr &&
           signature.key == nullptr && signature.value == nullptr &&
           signature.weak_target == nullptr &&
           signature.function_parameters.empty() && !signature.named_type.has_value() &&
           !signature.record_layout.has_value() &&
           !signature.variant_layout.has_value();
}

bool is_valid_map_key_type(const SignatureValue& key) {
    return key.kind == ValueKind::Int64 || key.kind == ValueKind::Bool ||
           key.kind == ValueKind::Str;
}

bool signature_has_invalid_map_key(const SignatureValue& signature) {
    if (signature.has_map_entries()) {
        return !is_valid_map_key_type(*signature.key) ||
               signature_has_invalid_map_key(*signature.key) ||
               signature_has_invalid_map_key(*signature.value);
    }
    if (signature.has_pair_fields()) {
        return signature_has_invalid_map_key(*signature.left) ||
               signature_has_invalid_map_key(*signature.right);
    }
    if (signature.has_array_element()) {
        return signature_has_invalid_map_key(*signature.element);
    }
    if (signature.has_weak_target()) {
        return signature_has_invalid_map_key(*signature.weak_target);
    }
    if (signature.has_function_signature()) {
        if (signature_has_invalid_map_key(*signature.function_return)) {
            return true;
        }
        for (const auto& parameter : signature.function_parameters) {
            if (signature_has_invalid_map_key(parameter)) {
                return true;
            }
        }
    }
    return false;
}

bool signature_has_invalid_weak_target(const SignatureValue& signature) {
    if (signature.has_weak_target()) {
        return !signature_value_is_reference(*signature.weak_target) ||
               signature_has_invalid_weak_target(*signature.weak_target);
    }
    if (signature.kind == ValueKind::Weak) {
        return true;
    }
    if (signature.has_pair_fields()) {
        return signature_has_invalid_weak_target(*signature.left) ||
               signature_has_invalid_weak_target(*signature.right);
    }
    if (signature.has_array_element()) {
        return signature_has_invalid_weak_target(*signature.element);
    }
    if (signature.has_map_entries()) {
        return signature_has_invalid_weak_target(*signature.key) ||
               signature_has_invalid_weak_target(*signature.value);
    }
    if (signature.has_function_signature()) {
        if (signature_has_invalid_weak_target(*signature.function_return)) {
            return true;
        }
        for (const auto& parameter : signature.function_parameters) {
            if (signature_has_invalid_weak_target(parameter)) {
                return true;
            }
        }
    }
    return false;
}

bool module_weak_target_types_are_valid(
    const Module& module, std::vector<VerifierDiagnostic>& diagnostics) {
    const auto reject_invalid = [&](std::size_t function_index,
                                    const SignatureValue& signature,
                                    std::string message) {
        if (!signature_has_invalid_weak_target(signature)) {
            return false;
        }
        reject(diagnostics, function_index, std::nullopt,
               VerifierReason::BadWeakTargetType, std::move(message));
        return true;
    };

    for (const auto& type : module.named_types) {
        if (reject_invalid(0, type.body,
                           "module named type contains a non-object weak target")) {
            return false;
        }
    }
    for (std::size_t function_index = 0;
         function_index < module.functions.size(); ++function_index) {
        const auto& signature = module.functions[function_index].signature;
        for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
            if (reject_invalid(function_index,
                               parameter_signature(signature, i),
                               "function parameter contains a non-object weak target")) {
                return false;
            }
        }
        if (reject_invalid(function_index, return_signature(signature),
                           "function return contains a non-object weak target")) {
            return false;
        }
    }
    for (const auto& layout : module.closure_layouts) {
        if (reject_invalid(layout.function_index, layout.function_type,
                           "closure function type contains a non-object weak target")) {
            return false;
        }
        for (const auto& capture : layout.capture_types) {
            if (reject_invalid(layout.function_index, capture,
                               "closure capture contains a non-object weak target")) {
                return false;
            }
        }
    }
    for (const auto& layout : module.map_layouts) {
        if (reject_invalid(0, layout.key_type,
                           "map key contains a non-object weak target") ||
            reject_invalid(0, layout.value_type,
                           "map value contains a non-object weak target")) {
            return false;
        }
    }
    for (const auto& layout : module.record_layouts) {
        for (const auto& field : layout.field_types) {
            if (reject_invalid(0, field,
                               "record field contains a non-object weak target")) {
                return false;
            }
        }
    }
    for (const auto& layout : module.variant_layouts) {
        for (const auto& case_layout : layout.cases) {
            for (const auto& field : case_layout.field_types) {
                if (reject_invalid(
                        0, field,
                        "variant field contains a non-object weak target")) {
                    return false;
                }
            }
        }
    }
    return true;
}

bool module_map_key_types_are_valid(
    const Module& module, std::vector<VerifierDiagnostic>& diagnostics) {
    for (const auto& type : module.named_types) {
        if (signature_has_invalid_map_key(type.body)) {
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::InvalidMapKeyType,
                          "module named type contains a map key other than i64, bool, or str");
        }
    }
    for (std::size_t function_index = 0;
         function_index < module.functions.size(); ++function_index) {
        const auto& signature = module.functions[function_index].signature;
        for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
            if (signature_has_invalid_map_key(parameter_signature(signature, i))) {
                return reject(diagnostics, function_index, std::nullopt,
                              VerifierReason::InvalidMapKeyType,
                              "function parameter contains a map key other than i64, bool, or str");
            }
        }
        if (signature_has_invalid_map_key(return_signature(signature))) {
            return reject(diagnostics, function_index, std::nullopt,
                          VerifierReason::InvalidMapKeyType,
                          "function return contains a map key other than i64, bool, or str");
        }
    }
    for (const auto& layout : module.closure_layouts) {
        if (signature_has_invalid_map_key(layout.function_type)) {
            return reject(diagnostics, layout.function_index, std::nullopt,
                          VerifierReason::InvalidMapKeyType,
                          "closure function type contains an invalid map key");
        }
        for (const auto& capture : layout.capture_types) {
            if (signature_has_invalid_map_key(capture)) {
                return reject(diagnostics, layout.function_index, std::nullopt,
                              VerifierReason::InvalidMapKeyType,
                              "closure capture type contains an invalid map key");
            }
        }
    }
    for (const auto& layout : module.map_layouts) {
        if (!is_valid_map_key_type(layout.key_type) ||
            signature_has_invalid_map_key(layout.value_type)) {
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::InvalidMapKeyType,
                          "map layout contains a key type other than i64, bool, or str");
        }
    }
    for (const auto& layout : module.record_layouts) {
        for (const auto& field : layout.field_types) {
            if (signature_has_invalid_map_key(field)) {
                return reject(diagnostics, 0, std::nullopt,
                              VerifierReason::InvalidMapKeyType,
                              "record field contains a map key other than i64, bool, or str");
            }
        }
    }
    for (const auto& layout : module.variant_layouts) {
        for (const auto& case_layout : layout.cases) {
            for (const auto& field : case_layout.field_types) {
                if (signature_has_invalid_map_key(field)) {
                    return reject(
                        diagnostics, 0, std::nullopt,
                        VerifierReason::InvalidMapKeyType,
                        "variant field contains a map key other than i64, bool, or str");
                }
            }
        }
    }
    return true;
}

bool record_layouts_are_well_formed(
    const Module& module, std::vector<VerifierDiagnostic>& diagnostics) {
    std::set<std::string> names;
    for (std::size_t i = 0; i < module.record_layouts.size(); ++i) {
        const auto& layout = module.record_layouts[i];
        if (layout.name.empty() || !names.insert(layout.name).second ||
            layout.field_types.size() != layout.reference_map.size()) {
            std::ostringstream message;
            message << "record layout " << i
                    << " has an empty/duplicate name or mismatched field bitmap";
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::BadRecordLayoutShape,
                          message.str());
        }
        for (std::size_t field = 0; field < layout.field_types.size(); ++field) {
            const auto& type = layout.field_types[field];
            if (!signature_shape_matches_kind(type, type.kind, module) ||
                layout.reference_map[field] !=
                    signature_value_is_reference(type)) {
                std::ostringstream message;
                message << "record layout " << i << " field " << field
                        << " disagrees with its signature or derived reference bitmap";
                return reject(diagnostics, 0, std::nullopt,
                              VerifierReason::BadRecordLayoutShape,
                              message.str());
            }
        }
    }
    return true;
}

bool variant_layouts_are_well_formed(
    const Module& module, std::vector<VerifierDiagnostic>& diagnostics) {
    std::set<std::string> layout_names;
    for (std::size_t layout_index = 0;
         layout_index < module.variant_layouts.size(); ++layout_index) {
        const auto& layout = module.variant_layouts[layout_index];
        if (layout.name.empty() ||
            !layout_names.insert(layout.name).second ||
            layout.cases.empty()) {
            std::ostringstream message;
            message << "variant layout " << layout_index
                    << " has an empty/duplicate name or no cases";
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::BadVariantLayoutShape,
                          message.str());
        }

        std::set<std::string> case_names;
        for (std::size_t case_index = 0;
             case_index < layout.cases.size(); ++case_index) {
            const auto& case_layout = layout.cases[case_index];
            if (case_layout.name.empty() ||
                !case_names.insert(case_layout.name).second ||
                case_layout.field_types.size() !=
                    case_layout.reference_map.size()) {
                std::ostringstream message;
                message << "variant layout " << layout_index << " case "
                        << case_index
                        << " has an empty/duplicate name or mismatched field bitmap";
                return reject(diagnostics, 0, std::nullopt,
                              VerifierReason::BadVariantLayoutShape,
                              message.str());
            }

            for (std::size_t field_index = 0;
                 field_index < case_layout.field_types.size(); ++field_index) {
                const auto& type = case_layout.field_types[field_index];
                if (!signature_shape_matches_kind(type, type.kind, module) ||
                    signature_has_invalid_map_key(type) ||
                    signature_has_invalid_weak_target(type) ||
                    case_layout.reference_map[field_index] !=
                        signature_value_is_reference(type)) {
                    std::ostringstream message;
                    message << "variant layout " << layout_index << " case "
                            << case_index << " field " << field_index
                            << " disagrees with its signature or derived reference bitmap";
                    return reject(diagnostics, 0, std::nullopt,
                                  VerifierReason::BadVariantLayoutShape,
                                  message.str());
                }
            }
        }
    }
    return true;
}

bool map_layouts_are_well_formed(
    const Module& module, std::vector<VerifierDiagnostic>& diagnostics) {
    for (std::size_t i = 0; i < module.map_layouts.size(); ++i) {
        const auto& layout = module.map_layouts[i];
        if (!is_valid_map_key_type(layout.key_type)) {
            std::ostringstream message;
            message << "map layout " << i
                    << " key type must be i64, bool, or str";
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::InvalidMapKeyType, message.str());
        }
        if (!signature_shape_matches_kind(layout.key_type,
                                          layout.key_type.kind, module) ||
            !signature_shape_matches_kind(layout.value_type,
                                          layout.value_type.kind, module) ||
            layout.key_is_ref !=
                signature_value_is_reference(layout.key_type) ||
            layout.value_is_ref !=
                signature_value_is_reference(layout.value_type)) {
            std::ostringstream message;
            message << "map layout " << i
                    << " type shape or derived reference flags are invalid";
            return reject(diagnostics, 0, std::nullopt,
                          VerifierReason::BadMapLayoutIndex, message.str());
        }
    }
    return true;
}

bool named_types_are_well_formed(const Module& module) {
    for (const auto& type : module.named_types) {
        if (!type.body.has_pair_fields()) {
            return false;
        }
        if (!signature_shape_matches_kind(type.body, ValueKind::Object, module)) {
            return false;
        }
    }
    return true;
}

bool signature_is_well_formed(const FunctionSignature& signature,
                              const Module& module) {
    if (!signature.parameter_types.empty() &&
        signature.parameter_types.size() != signature.parameters.size()) {
        return false;
    }
    for (std::size_t i = 0; i < signature.parameters.size(); ++i) {
        if (signature.parameters[i] == ValueKind::Record &&
            (signature.parameter_types.empty() ||
             !signature.parameter_types[i].is_record_layout_reference())) {
            return false;
        }
        if (signature.parameters[i] == ValueKind::Variant &&
            (signature.parameter_types.empty() ||
             !signature.parameter_types[i].is_variant_layout_reference())) {
            return false;
        }
    }
    for (std::size_t i = 0; i < signature.parameter_types.size(); ++i) {
        if (!signature_shape_matches_kind(signature.parameter_types[i],
                                          signature.parameters[i], module)) {
            return false;
        }
    }
    if (signature.return_type == ValueKind::Record &&
        (!signature.return_type_detail.has_value() ||
         !signature.return_type_detail->is_record_layout_reference())) {
        return false;
    }
    if (signature.return_type == ValueKind::Variant &&
        (!signature.return_type_detail.has_value() ||
         !signature.return_type_detail->is_variant_layout_reference())) {
        return false;
    }
    if (signature.return_type_detail.has_value() &&
        !signature_shape_matches_kind(*signature.return_type_detail,
                                      signature.return_type, module)) {
        return false;
    }
    return true;
}

bool closure_layouts_are_well_formed(
    const Module& module, std::vector<VerifierDiagnostic>& diagnostics) {
    for (std::size_t i = 0; i < module.closure_layouts.size(); ++i) {
        const auto& layout = module.closure_layouts[i];
        if (layout.function_index >= module.functions.size() ||
            !module.functions[layout.function_index].closure_layout.has_value() ||
            *module.functions[layout.function_index].closure_layout != i ||
            !layout.function_type.has_function_signature() ||
            !signature_shape_matches_kind(layout.function_type,
                                          ValueKind::Function, module) ||
            !signature_is_well_formed(
                module.functions[layout.function_index].signature, module) ||
            !signature_values_equal(
                layout.function_type,
                function_type_from_signature(
                    module.functions[layout.function_index].signature))) {
            std::ostringstream message;
            message << "closure layout " << i
                    << " has an invalid function target or structural signature";
            return reject(diagnostics, layout.function_index, std::nullopt,
                          VerifierReason::BadClosureLayoutIndex,
                          message.str());
        }
        if (layout.capture_types.size() != layout.capture_map.size()) {
            std::ostringstream message;
            message << "closure layout " << i << " capture type count "
                    << layout.capture_types.size() << " differs from capture map count "
                    << layout.capture_map.size();
            return reject(diagnostics, layout.function_index, std::nullopt,
                          VerifierReason::BadClosureCaptureArity,
                          message.str());
        }
        for (std::size_t capture = 0; capture < layout.capture_types.size(); ++capture) {
            const auto& type = layout.capture_types[capture];
            if (!signature_shape_matches_kind(type, type.kind, module) ||
                layout.capture_map[capture] != signature_value_is_reference(type)) {
                std::ostringstream message;
                message << "closure layout " << i << " capture " << capture
                        << " disagrees with its derived reference bitmap";
                return reject(diagnostics, layout.function_index, std::nullopt,
                              VerifierReason::BadClosureCaptureType,
                              message.str());
            }
        }
    }
    for (std::size_t function_index = 0;
         function_index < module.functions.size(); ++function_index) {
        const auto& function = module.functions[function_index];
        if (!function.closure_layout.has_value()) {
            continue;
        }
        if (*function.closure_layout >= module.closure_layouts.size() ||
            module.closure_layouts[*function.closure_layout].function_index !=
                function_index) {
            return reject(diagnostics, function_index, std::nullopt,
                          VerifierReason::BadClosureLayoutIndex,
                          "function closure-body metadata does not name its own layout");
        }
    }
    return true;
}

bool fields_equal(const std::shared_ptr<const PairFields>& lhs,
                  const std::shared_ptr<const PairFields>& rhs);
bool abstract_value_ptr_equal(const std::shared_ptr<const AbstractValue>& lhs,
                              const std::shared_ptr<const AbstractValue>& rhs);

bool operator==(const AbstractValue& lhs, const AbstractValue& rhs) {
    return lhs.kind == rhs.kind && lhs.object_sites == rhs.object_sites &&
           lhs.ref_array_sites == rhs.ref_array_sites &&
           lhs.includes_opaque_object == rhs.includes_opaque_object &&
           lhs.includes_nil == rhs.includes_nil &&
           fields_equal(lhs.signature_fields, rhs.signature_fields) &&
           abstract_value_ptr_equal(lhs.signature_array_element,
                                    rhs.signature_array_element) &&
           ((lhs.signature_function == nullptr &&
             rhs.signature_function == nullptr) ||
            (lhs.signature_function != nullptr &&
             rhs.signature_function != nullptr &&
             signature_values_equal(*lhs.signature_function,
                                    *rhs.signature_function))) &&
           ((lhs.signature_map == nullptr && rhs.signature_map == nullptr) ||
            (lhs.signature_map != nullptr && rhs.signature_map != nullptr &&
             signature_values_equal(*lhs.signature_map,
                                    *rhs.signature_map))) &&
           abstract_value_ptr_equal(lhs.reference_target,
                                    rhs.reference_target) &&
           lhs.signature_named_type == rhs.signature_named_type &&
           lhs.signature_record_layout == rhs.signature_record_layout &&
           lhs.signature_variant_layout == rhs.signature_variant_layout &&
           lhs.source_local == rhs.source_local &&
           lhs.nil_test_local == rhs.nil_test_local;
}

bool operator==(const PairFields& lhs, const PairFields& rhs) {
    return lhs.left == rhs.left && lhs.right == rhs.right;
}

bool fields_equal(const std::shared_ptr<const PairFields>& lhs,
                  const std::shared_ptr<const PairFields>& rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    return *lhs == *rhs;
}

bool abstract_value_ptr_equal(const std::shared_ptr<const AbstractValue>& lhs,
                              const std::shared_ptr<const AbstractValue>& rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return lhs == rhs;
    }
    return *lhs == *rhs;
}

bool is_valid_local_index(std::int64_t operand, std::uint32_t local_count) {
    return operand >= 0 && static_cast<std::uint64_t>(operand) < local_count;
}

bool is_valid_target(std::int64_t operand, std::size_t code_size) {
    return operand >= 0 && static_cast<std::uint64_t>(operand) < code_size;
}

bool is_poison(const AbstractValue& value) {
    return value.kind == AbstractKind::Poison;
}

bool is_canonical_nil_object(const AbstractValue& value) {
    return value.kind == AbstractKind::Object && value.includes_nil &&
           value.object_sites.empty() && value.ref_array_sites.empty() &&
           !value.includes_opaque_object && value.signature_fields == nullptr &&
           value.signature_array_element == nullptr &&
           value.signature_function == nullptr && value.signature_map == nullptr &&
           value.reference_target == nullptr &&
           !value.signature_named_type.has_value() &&
           !value.signature_record_layout.has_value() &&
           !value.signature_variant_layout.has_value();
}

AbstractValue join_values(const AbstractValue& lhs, const AbstractValue& rhs) {
    const auto join_maybe_with_target = [](const AbstractValue& maybe,
                                           const AbstractValue& target)
        -> std::optional<AbstractValue> {
        if (maybe.kind != AbstractKind::MaybeReference ||
            maybe.reference_target == nullptr ||
            maybe.reference_target->kind != target.kind) {
            return std::nullopt;
        }
        auto joined_target = join_values(*maybe.reference_target, target);
        if (is_poison(joined_target)) {
            return poison_value();
        }
        return maybe_reference_value(std::move(joined_target));
    };
    if (auto joined = join_maybe_with_target(lhs, rhs); joined.has_value()) {
        return *joined;
    }
    if (auto joined = join_maybe_with_target(rhs, lhs); joined.has_value()) {
        return *joined;
    }
    const auto join_nominal_with_nil = [](const AbstractValue& nominal,
                                          const AbstractValue& nil) {
        auto joined = nominal;
        joined.includes_nil = true;
        if (nominal.source_local != nil.source_local) {
            joined.source_local.reset();
        }
        if (nominal.nil_test_local != nil.nil_test_local) {
            joined.nil_test_local.reset();
        }
        return joined;
    };
    if (lhs.kind == AbstractKind::Record && is_canonical_nil_object(rhs)) {
        return join_nominal_with_nil(lhs, rhs);
    }
    if (rhs.kind == AbstractKind::Record && is_canonical_nil_object(lhs)) {
        return join_nominal_with_nil(rhs, lhs);
    }
    if (lhs.kind == AbstractKind::Variant && is_canonical_nil_object(rhs)) {
        return join_nominal_with_nil(lhs, rhs);
    }
    if (rhs.kind == AbstractKind::Variant && is_canonical_nil_object(lhs)) {
        return join_nominal_with_nil(rhs, lhs);
    }
    if (lhs.kind != rhs.kind || is_poison(lhs) || is_poison(rhs)) {
        return poison_value();
    }

    AbstractValue joined = lhs;
    if (joined.kind == AbstractKind::Object) {
        joined.object_sites.insert(rhs.object_sites.begin(), rhs.object_sites.end());
        joined.includes_opaque_object =
            lhs.includes_opaque_object || rhs.includes_opaque_object;
        joined.includes_nil = lhs.includes_nil || rhs.includes_nil;
        if (lhs.signature_named_type == rhs.signature_named_type) {
            joined.signature_named_type = lhs.signature_named_type;
        } else if (lhs.signature_named_type.has_value() &&
                   !rhs.signature_named_type.has_value()) {
            joined.signature_named_type = lhs.signature_named_type;
        } else if (!lhs.signature_named_type.has_value() &&
                   rhs.signature_named_type.has_value()) {
            joined.signature_named_type = rhs.signature_named_type;
        } else {
            joined.signature_named_type.reset();
        }
        if (lhs.signature_fields != nullptr && rhs.signature_fields != nullptr) {
            joined.signature_fields = fields_ptr(PairFields{
                join_values(lhs.signature_fields->left, rhs.signature_fields->left),
                join_values(lhs.signature_fields->right, rhs.signature_fields->right),
            });
        } else if (lhs.signature_fields != nullptr) {
            joined.signature_fields = lhs.signature_fields;
        } else {
            joined.signature_fields = rhs.signature_fields;
        }
    } else if (joined.kind == AbstractKind::RefArray) {
        joined.ref_array_sites.insert(rhs.ref_array_sites.begin(),
                                      rhs.ref_array_sites.end());
        if (lhs.signature_array_element != nullptr &&
            rhs.signature_array_element != nullptr) {
            joined.signature_array_element = abstract_value_ptr(
                join_values(*lhs.signature_array_element,
                            *rhs.signature_array_element));
        } else if (lhs.signature_array_element != nullptr) {
            joined.signature_array_element = lhs.signature_array_element;
        } else {
            joined.signature_array_element = rhs.signature_array_element;
        }
    } else if (joined.kind == AbstractKind::Function) {
        if (lhs.signature_function == nullptr ||
            rhs.signature_function == nullptr ||
            !signature_values_equal(*lhs.signature_function,
                                    *rhs.signature_function)) {
            return poison_value();
        }
    } else if (joined.kind == AbstractKind::Map) {
        if (lhs.signature_map == nullptr || rhs.signature_map == nullptr ||
            !signature_values_equal(*lhs.signature_map,
                                    *rhs.signature_map)) {
            return poison_value();
        }
    } else if (joined.kind == AbstractKind::Record) {
        if (!lhs.signature_record_layout.has_value() ||
            lhs.signature_record_layout != rhs.signature_record_layout) {
            return poison_value();
        }
        joined.includes_nil = lhs.includes_nil || rhs.includes_nil;
    } else if (joined.kind == AbstractKind::Variant) {
        if (!lhs.signature_variant_layout.has_value() ||
            lhs.signature_variant_layout != rhs.signature_variant_layout) {
            return poison_value();
        }
        joined.includes_nil = lhs.includes_nil || rhs.includes_nil;
    } else if (joined.kind == AbstractKind::Weak ||
               joined.kind == AbstractKind::MaybeReference) {
        if (lhs.reference_target == nullptr ||
            rhs.reference_target == nullptr) {
            return poison_value();
        }
        auto target = join_values(*lhs.reference_target,
                                  *rhs.reference_target);
        if (is_poison(target)) {
            return poison_value();
        }
        joined.reference_target = abstract_value_ptr(std::move(target));
    }
    if (lhs.source_local == rhs.source_local) {
        joined.source_local = lhs.source_local;
    } else {
        joined.source_local.reset();
    }
    if (lhs.nil_test_local == rhs.nil_test_local) {
        joined.nil_test_local = lhs.nil_test_local;
    } else {
        joined.nil_test_local.reset();
    }
    return joined;
}

bool join_value_into(AbstractValue& destination, const AbstractValue& incoming) {
    const auto joined = join_values(destination, incoming);
    if (joined == destination) {
        return false;
    }
    destination = joined;
    return true;
}

AbstractValue join_ref_array_element_values(const AbstractValue& lhs,
                                            const AbstractValue& rhs) {
    const auto joined = join_values(lhs, rhs);
    if (!is_poison(joined)) {
        return joined;
    }
    if (lhs.kind == AbstractKind::Function ||
        rhs.kind == AbstractKind::Function) {
        return joined;
    }
    if (is_reference_kind(lhs.kind) && is_reference_kind(rhs.kind)) {
        return opaque_object_value();
    }
    return joined;
}

bool join_ref_array_element_into(AbstractValue& destination,
                                 const AbstractValue& incoming) {
    const auto joined = join_ref_array_element_values(destination, incoming);
    if (joined == destination) {
        return false;
    }
    destination = joined;
    return true;
}

JoinOutcome join_state_into(AbstractState& destination, const AbstractState& incoming) {
    if (destination.stack.size() != incoming.stack.size() ||
        destination.locals.size() != incoming.locals.size() ||
        destination.local_reference_kinds.size() !=
            incoming.local_reference_kinds.size()) {
        return JoinOutcome::Invalid;
    }

    bool changed = false;

    for (std::size_t i = 0; i < destination.stack.size(); ++i) {
        changed = join_value_into(destination.stack[i], incoming.stack[i]) || changed;
    }

    for (std::size_t i = 0; i < destination.locals.size(); ++i) {
        auto& dest_local = destination.locals[i];
        const auto& incoming_local = incoming.locals[i];
        if (!dest_local.has_value() && !incoming_local.has_value()) {
            continue;
        }
        if (!dest_local.has_value() || !incoming_local.has_value()) {
            if (dest_local.has_value()) {
                dest_local.reset();
                changed = true;
            }
            continue;
        }
        changed = join_value_into(*dest_local, *incoming_local) || changed;
    }

    for (std::size_t i = 0; i < destination.local_reference_kinds.size(); ++i) {
        auto& destination_kind = destination.local_reference_kinds[i];
        const auto incoming_kind = incoming.local_reference_kinds[i];
        if (!incoming_kind.has_value()) {
            continue;
        }
        if (!destination_kind.has_value()) {
            destination_kind = incoming_kind;
            changed = true;
            continue;
        }
        if (*destination_kind != *incoming_kind) {
            return JoinOutcome::Invalid;
        }
    }

    for (const auto& [site, incoming_fields] : incoming.fields_by_site) {
        auto [it, inserted] = destination.fields_by_site.emplace(site, incoming_fields);
        if (inserted) {
            changed = true;
            continue;
        }
        changed = join_value_into(it->second.left, incoming_fields.left) || changed;
        changed = join_value_into(it->second.right, incoming_fields.right) || changed;
    }

    for (const auto& [site, incoming_element] : incoming.ref_array_elements_by_site) {
        auto [it, inserted] =
            destination.ref_array_elements_by_site.emplace(site, incoming_element);
        if (inserted) {
            changed = true;
            continue;
        }
        changed = join_ref_array_element_into(it->second, incoming_element) || changed;
    }

    return changed ? JoinOutcome::Changed : JoinOutcome::Unchanged;
}

bool stack_map_matches(const StackMap& map, const AbstractState& state) {
    if (map.object_slots.size() != state.stack.size()) {
        return false;
    }
    for (std::size_t i = 0; i < state.stack.size(); ++i) {
        if (is_poison(state.stack[i])) {
            return false;
        }
        const bool is_object = is_reference_kind(state.stack[i].kind);
        if (map.object_slots[i] != is_object) {
            return false;
        }
    }
    // An empty local map is the legacy hand-written format. Generated maps always carry
    // exact local bits; a supplied non-empty map must agree completely.
    if (!map.local_object_slots.empty()) {
        if (map.local_object_slots.size() != state.locals.size()) {
            return false;
        }
        for (std::size_t i = 0; i < state.locals.size(); ++i) {
            const bool is_object =
                state.local_reference_kinds[i].value_or(false);
            if (map.local_object_slots[i] != is_object) {
                return false;
            }
        }
    }
    return true;
}

std::optional<StackMap> stack_map_from_state(const AbstractState& state) {
    StackMap map;
    map.object_slots.reserve(state.stack.size());
    for (const auto& value : state.stack) {
        if (is_poison(value)) {
            return std::nullopt;
        }
        map.object_slots.push_back(is_reference_kind(value.kind));
    }
    map.local_object_slots.reserve(state.locals.size());
    for (const auto local_kind : state.local_reference_kinds) {
        map.local_object_slots.push_back(local_kind.value_or(false));
    }
    return map;
}

bool pop_any_or_report(AbstractState& state,
                       std::vector<VerifierDiagnostic>& diagnostics,
                       const Function& function,
                       std::size_t function_index,
                       std::size_t pc,
                       VerifierReason underflow_reason,
                       std::string_view context,
                       AbstractValue* out = nullptr) {
    if (state.stack.empty()) {
        std::ostringstream message;
        message << context << " requires a stack value";
        return reject(diagnostics, function_index, pc, underflow_reason,
                      instruction_message(function, pc, message.str()));
    }
    const auto& value = state.stack.back();
    if (is_poison(value)) {
        std::ostringstream message;
        message << context << " consumed a poison value";
        return reject(diagnostics, function_index, pc, VerifierReason::PoisonUse,
                      instruction_message(function, pc, message.str()));
    }
    if (out != nullptr) {
        *out = std::move(state.stack.back());
    }
    state.stack.pop_back();
    return true;
}

bool pop_expect_or_report(AbstractState& state,
                          std::vector<VerifierDiagnostic>& diagnostics,
                          const Function& function,
                          std::size_t function_index,
                          std::size_t pc,
                          AbstractKind expected,
                          VerifierReason underflow_reason,
                          VerifierReason mismatch_reason,
                          std::string_view context,
                          AbstractValue* out = nullptr) {
    AbstractValue actual;
    if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                           underflow_reason, context, &actual)) {
        return false;
    }
    if (actual.kind != expected) {
        std::ostringstream message;
        message << context << " expected " << abstract_kind_name(expected)
                << " but found " << abstract_kind_name(actual.kind);
        const auto reason = actual.kind == AbstractKind::MaybeReference
                                ? VerifierReason::WeakTargetMayBeNil
                                : mismatch_reason;
        return reject(diagnostics, function_index, pc, reason,
                      instruction_message(function, pc, message.str()));
    }
    if (out != nullptr) {
        *out = std::move(actual);
    }
    return true;
}

bool pop_reference_or_report(AbstractState& state,
                             std::vector<VerifierDiagnostic>& diagnostics,
                             const Function& function,
                             std::size_t function_index,
                             std::size_t pc,
                             VerifierReason underflow_reason,
                             VerifierReason mismatch_reason,
                             std::string_view context,
                             AbstractValue* out = nullptr) {
    AbstractValue actual;
    if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                           underflow_reason, context, &actual)) {
        return false;
    }
    if (!is_reference_kind(actual.kind) ||
        actual.kind == AbstractKind::MaybeReference || actual.includes_nil) {
        std::ostringstream message;
        message << context << " expected a non-nil reference but found "
                << abstract_kind_name(actual.kind);
        return reject(diagnostics, function_index, pc, mismatch_reason,
                      instruction_message(function, pc, message.str()));
    }
    if (out != nullptr) {
        *out = std::move(actual);
    }
    return true;
}

bool push_fallthrough_or_report(
    std::size_t pc, const Function& function, std::size_t function_index,
    AbstractState&& state,
    std::vector<std::pair<std::size_t, AbstractState>>& successors,
    std::vector<VerifierDiagnostic>& diagnostics) {
    const auto next_pc = pc + 1;
    if (next_pc >= function.code.size()) {
        return reject(diagnostics, function_index, pc, VerifierReason::FallOffEnd,
                      instruction_message(function, pc,
                                          "instruction would fall through past function end"));
    }
    successors.emplace_back(next_pc, std::move(state));
    return true;
}

const ExceptionHandler* enclosing_handler(const Function& function,
                                          std::size_t pc,
                                          std::optional<std::size_t> layout = std::nullopt) {
    const ExceptionHandler* selected = nullptr;
    for (const auto& handler : function.exception_handlers) {
        if (handler.try_begin < pc && pc < handler.try_end &&
            (!layout.has_value() || handler.variant_layout == *layout) &&
            (selected == nullptr || handler.try_begin > selected->try_begin)) {
            selected = &handler;
        }
    }
    return selected;
}

void add_exception_successor(const ExceptionHandler& handler,
                             AbstractState state,
                             std::vector<std::pair<std::size_t, AbstractState>>& successors) {
    state.stack.clear();
    state.stack.push_back(variant_value(handler.variant_layout, false));
    successors.emplace_back(handler.target, std::move(state));
}

bool load_pair_field(const Module& module, const AbstractState& state,
                     const AbstractValue& receiver, bool left, AbstractValue& out) {
    if (receiver.includes_nil || receiver.includes_opaque_object) {
        return false;
    }

    std::optional<AbstractValue> loaded;
    if (receiver.signature_fields != nullptr) {
        loaded = left ? receiver.signature_fields->left : receiver.signature_fields->right;
    }
    if (receiver.signature_named_type.has_value()) {
        if (*receiver.signature_named_type >= module.named_types.size()) {
            return false;
        }
        const auto named_body =
            value_from_signature(module.named_types[*receiver.signature_named_type].body);
        if (named_body.signature_fields == nullptr) {
            return false;
        }
        const auto& field =
            left ? named_body.signature_fields->left : named_body.signature_fields->right;
        if (!loaded.has_value()) {
            loaded = field;
        } else {
            *loaded = join_values(*loaded, field);
        }
    }

    for (const auto site : receiver.object_sites) {
        const auto it = state.fields_by_site.find(site);
        if (it == state.fields_by_site.end()) {
            return false;
        }
        const auto& field = left ? it->second.left : it->second.right;
        if (!loaded.has_value()) {
            loaded = field;
        } else {
            *loaded = join_values(*loaded, field);
        }
    }
    if (!loaded.has_value() || is_poison(*loaded)) {
        return false;
    }
    out = *loaded;
    return true;
}

struct NonnilValueShape {
    std::vector<std::size_t> object_sites;
    bool includes_opaque_object{false};
    std::uintptr_t signature_fields{0};
    std::optional<std::size_t> signature_named_type;

    [[nodiscard]] bool operator<(const NonnilValueShape& other) const {
        if (object_sites != other.object_sites) {
            return object_sites < other.object_sites;
        }
        if (includes_opaque_object != other.includes_opaque_object) {
            return includes_opaque_object < other.includes_opaque_object;
        }
        if (signature_fields != other.signature_fields) {
            return signature_fields < other.signature_fields;
        }
        return signature_named_type < other.signature_named_type;
    }
};

struct ConformanceAssumption {
    NonnilValueShape value;
    std::size_t expected_named_type{0};

    [[nodiscard]] bool operator<(const ConformanceAssumption& other) const {
        if (value < other.value) {
            return true;
        }
        if (other.value < value) {
            return false;
        }
        return expected_named_type < other.expected_named_type;
    }
};

NonnilValueShape nonnil_shape(const AbstractValue& value) {
    return NonnilValueShape{
        std::vector<std::size_t>(value.object_sites.begin(), value.object_sites.end()),
        value.includes_opaque_object,
        reinterpret_cast<std::uintptr_t>(value.signature_fields.get()),
        value.signature_named_type,
    };
}

bool has_nonnil_object_evidence(const AbstractValue& value) {
    return value.includes_opaque_object || !value.object_sites.empty() ||
           value.signature_fields != nullptr || value.signature_named_type.has_value();
}

bool value_conforms_to_expected(const Module& module, const AbstractState& state,
                                const AbstractValue& value,
                                const AbstractValue& expected,
                                std::set<ConformanceAssumption>& assumptions);

bool load_ref_array_element(const AbstractState& state, const AbstractValue& receiver,
                            AbstractValue& out) {
    std::optional<AbstractValue> loaded;
    if (receiver.signature_array_element != nullptr) {
        loaded = *receiver.signature_array_element;
    }
    for (const auto site : receiver.ref_array_sites) {
        const auto it = state.ref_array_elements_by_site.find(site);
        if (it == state.ref_array_elements_by_site.end()) {
            return false;
        }
        if (!loaded.has_value()) {
            loaded = it->second;
        } else {
            *loaded = join_ref_array_element_values(*loaded, it->second);
        }
    }
    if (!loaded.has_value() || is_poison(*loaded)) {
        return false;
    }
    out = *loaded;
    return true;
}

bool value_conforms_to_signature(const Module& module, const AbstractState& state,
                                 const AbstractValue& value,
                                 const SignatureValue& signature) {
    std::set<ConformanceAssumption> assumptions;
    return value_conforms_to_expected(module, state, value,
                                      value_from_signature(signature), assumptions);
}

bool value_conforms_to_expected(const Module& module, const AbstractState& state,
                                const AbstractValue& value,
                                const AbstractValue& expected,
                                std::set<ConformanceAssumption>& assumptions) {
    if (is_poison(value) || is_poison(expected)) {
        return false;
    }
    if (expected.kind == AbstractKind::Nil && is_canonical_nil_object(value)) {
        return true;
    }
    if (expected.kind == AbstractKind::Record &&
        is_canonical_nil_object(value)) {
        return expected.includes_nil;
    }
    if (expected.kind == AbstractKind::Variant &&
        is_canonical_nil_object(value)) {
        return expected.includes_nil;
    }
    if (value.kind == AbstractKind::MaybeReference &&
        value.reference_target != nullptr &&
        expected.kind == AbstractKind::Object &&
        expected.signature_named_type.has_value()) {
        return value_conforms_to_expected(module, state,
                                          *value.reference_target, expected,
                                          assumptions);
    }
    if (value.kind == AbstractKind::RefArray && expected.kind == AbstractKind::Object) {
        return !expected.includes_nil && !expected.signature_named_type.has_value() &&
               expected.signature_fields == nullptr;
    }
    if (value.kind != expected.kind) {
        return false;
    }
    if (expected.kind == AbstractKind::Weak) {
        return value.reference_target != nullptr &&
               expected.reference_target != nullptr &&
               value_conforms_to_expected(module, state,
                                          *value.reference_target,
                                          *expected.reference_target,
                                          assumptions);
    }
    if (expected.kind == AbstractKind::RefArray) {
        if (expected.signature_array_element == nullptr) {
            return true;
        }
        if (value.signature_array_element != nullptr &&
            !value_conforms_to_expected(module, state, *value.signature_array_element,
                                        *expected.signature_array_element,
                                        assumptions)) {
            return false;
        }
        for (const auto site : value.ref_array_sites) {
            const auto it = state.ref_array_elements_by_site.find(site);
            if (it == state.ref_array_elements_by_site.end() ||
                !value_conforms_to_expected(module, state, it->second,
                                            *expected.signature_array_element,
                                            assumptions)) {
                return false;
            }
        }
        return value.signature_array_element != nullptr ||
               !value.ref_array_sites.empty();
    }
    if (expected.kind == AbstractKind::Map) {
        return value.signature_map != nullptr &&
               expected.signature_map != nullptr &&
               signature_values_equal(*value.signature_map,
                                      *expected.signature_map);
    }
    if (expected.kind == AbstractKind::Function) {
        return value.signature_function != nullptr &&
               expected.signature_function != nullptr &&
               signature_values_equal(*value.signature_function,
                                      *expected.signature_function);
    }
    if (expected.kind == AbstractKind::Record) {
        return value.signature_record_layout.has_value() &&
               expected.signature_record_layout.has_value() &&
               value.signature_record_layout ==
                   expected.signature_record_layout &&
               (!value.includes_nil || expected.includes_nil);
    }
    if (expected.kind == AbstractKind::Variant) {
        return value.signature_variant_layout.has_value() &&
               expected.signature_variant_layout.has_value() &&
               value.signature_variant_layout ==
                   expected.signature_variant_layout &&
               (!value.includes_nil || expected.includes_nil);
    }
    if (expected.kind != AbstractKind::Object) {
        return true;
    }
    if (expected.signature_named_type.has_value()) {
        if (*expected.signature_named_type >= module.named_types.size()) {
            return false;
        }
        if (!has_nonnil_object_evidence(value)) {
            return value.includes_nil;
        }

        auto nonnil = value;
        nonnil.includes_nil = false;
        const ConformanceAssumption assumption{nonnil_shape(nonnil),
                                               *expected.signature_named_type};
        // Termination: recursive signatures can only re-enter through finite
        // Module::named_types references, and verifier values contain finite
        // allocation-site/signature evidence. Once the same non-nil value shape is
        // being checked against the same named type, the remaining obligation is the
        // same coinductive proof, so it is assumed instead of unfolded again.
        if (!assumptions.insert(assumption).second) {
            return true;
        }

        return value_conforms_to_expected(
            module, state, nonnil,
            value_from_signature(module.named_types[*expected.signature_named_type].body),
            assumptions);
    }
    if (value.includes_nil) {
        return false;
    }
    if (expected.signature_fields == nullptr) {
        return true;
    }

    AbstractValue value_left;
    AbstractValue value_right;
    return load_pair_field(module, state, value, true, value_left) &&
           load_pair_field(module, state, value, false, value_right) &&
           value_conforms_to_expected(module, state, value_left,
                                      expected.signature_fields->left, assumptions) &&
           value_conforms_to_expected(module, state, value_right,
                                      expected.signature_fields->right, assumptions);
}

bool store_pair_field(const Module& module, AbstractState& state,
                      const AbstractValue& receiver, bool left,
                      const AbstractValue& value) {
    if (receiver.includes_nil) {
        return false;
    }
    if (receiver.includes_opaque_object && receiver.signature_fields != nullptr) {
        return false;
    }

    std::optional<AbstractValue> expected;
    if (receiver.signature_fields != nullptr) {
        expected = left ? receiver.signature_fields->left : receiver.signature_fields->right;
    }
    if (receiver.signature_named_type.has_value()) {
        if (*receiver.signature_named_type >= module.named_types.size()) {
            return false;
        }
        const auto named_body =
            value_from_signature(module.named_types[*receiver.signature_named_type].body);
        if (named_body.signature_fields == nullptr) {
            return false;
        }
        const auto& named_expected =
            left ? named_body.signature_fields->left : named_body.signature_fields->right;
        if (!expected.has_value()) {
            expected = named_expected;
        } else {
            *expected = join_values(*expected, named_expected);
        }
    }
    if (expected.has_value()) {
        std::set<ConformanceAssumption> assumptions;
        if (!value_conforms_to_expected(module, state, value, *expected, assumptions)) {
            return false;
        }
    }

    for (const auto site : receiver.object_sites) {
        const auto it = state.fields_by_site.find(site);
        if (it == state.fields_by_site.end()) {
            return false;
        }
        auto& field = left ? it->second.left : it->second.right;
        (void)join_value_into(field, value);
    }
    return receiver.signature_fields != nullptr ||
           receiver.signature_named_type.has_value() || !receiver.object_sites.empty() ||
           receiver.includes_opaque_object;
}

bool store_ref_array_element(const Module& module, AbstractState& state,
                             const AbstractValue& receiver,
                             const AbstractValue& value) {
    if (receiver.signature_array_element != nullptr) {
        std::set<ConformanceAssumption> assumptions;
        if (!value_conforms_to_expected(module, state, value,
                                        *receiver.signature_array_element,
                                        assumptions)) {
            return false;
        }
    }
    for (const auto site : receiver.ref_array_sites) {
        const auto it = state.ref_array_elements_by_site.find(site);
        if (it == state.ref_array_elements_by_site.end()) {
            return false;
        }
        const auto joined = join_ref_array_element_values(it->second, value);
        if (is_poison(joined)) {
            return false;
        }
        it->second = joined;
    }
    return receiver.signature_array_element != nullptr ||
           !receiver.ref_array_sites.empty();
}

bool transfer_instruction(const Module& module, std::size_t function_index, std::size_t pc,
                          const AbstractState& in,
                          std::vector<std::pair<std::size_t, AbstractState>>& successors,
                          std::vector<VerifierDiagnostic>& diagnostics) {
    const auto& function = module.functions[function_index];
    const auto& ins = function.code[pc];
    auto state = in;

    switch (ins.op) {
    case OpCode::ConstantI64:
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::PushStr:
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.string_constants.size()) {
            std::ostringstream message;
            message << "string constant operand " << ins.operand
                    << " is outside pool size " << module.string_constants.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadStringConstantIndex,
                          instruction_message(function, pc, message.str()));
        }
        state.stack.push_back(str_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrLen:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "string receiver")) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrEq:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "right string operand") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "left string operand")) {
            return false;
        }
        state.stack.push_back(bool_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrConcat:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "right string operand") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "left string operand")) {
            return false;
        }
        state.stack.push_back(str_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrSub:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::StrSubRequiresI64Bounds,
                                  "substring high bound") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::StrSubRequiresI64Bounds,
                                  "substring low bound") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::StrSubRequiresStr,
                                  "substring receiver")) {
            return false;
        }
        state.stack.push_back(str_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrLt:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::StrLtRequiresStr,
                                  "right string operand") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::StrLtRequiresStr,
                                  "left string operand")) {
            return false;
        }
        state.stack.push_back(bool_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrIndex:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "string index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadStringOperation,
                                  "string receiver")) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::Print:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::PrintRequiresStr,
                                  "print operand")) {
            return false;
        }
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::I64ToStr:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::I64ToStrRequiresI64,
                                  "i64-to-string operand")) {
            return false;
        }
        state.stack.push_back(str_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::StrToI64:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Str,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::StrToI64RequiresStr,
                                  "string-to-i64 operand")) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::BoolToStr:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Bool,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BoolToStrRequiresBool,
                                  "bool-to-string operand")) {
            return false;
        }
        state.stack.push_back(str_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    case OpCode::Nil:
        state.stack.push_back(nil_object_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::AllocWeak: {
        AbstractValue target;
        if (!pop_reference_or_report(state, diagnostics, function,
                                     function_index, pc,
                                     VerifierReason::StackUnderflow,
                                     VerifierReason::BadWeakTargetType,
                                     "weak target", &target)) {
            return false;
        }
        state.stack.push_back(weak_value(std::move(target)));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::WeakGet: {
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index,
                               pc, VerifierReason::StackUnderflow,
                               "weak receiver", &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Weak ||
            receiver.reference_target == nullptr) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::WeakOperationOnNonWeak,
                          instruction_message(
                              function, pc,
                              "receiver is not a typed weak reference"));
        }
        state.stack.push_back(
            maybe_reference_value(*receiver.reference_target));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::IsNil: {
        AbstractValue value;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow,
                               "is_nil operand", &value)) {
            return false;
        }
        if (!is_reference_kind(value.kind)) {
            std::ostringstream message;
            message << "is_nil operand expected a reference but found "
                    << abstract_kind_name(value.kind);
            return reject(diagnostics, function_index, pc,
                          VerifierReason::TypeMismatch,
                          instruction_message(function, pc, message.str()));
        }
        auto result = bool_value();
        result.nil_test_local = value.source_local;
        state.stack.push_back(result);
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::AddI64:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "right operand") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "left operand")) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::LessI64:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "right operand") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "left operand")) {
            return false;
        }
        state.stack.push_back(bool_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::AllocPair: {
        AbstractValue right;
        AbstractValue left;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "right field", &right) ||
            !pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "left field", &left)) {
            return false;
        }
        const PairFields allocated{without_provenance(left), without_provenance(right)};
        auto [it, inserted] = state.fields_by_site.emplace(pc, allocated);
        if (!inserted) {
            (void)join_value_into(it->second.left, allocated.left);
            (void)join_value_into(it->second.right, allocated.right);
        }
        state.stack.push_back(object_value(pc));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::AllocRecord: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.record_layouts.size()) {
            std::ostringstream message;
            message << "record layout " << ins.operand
                    << " is outside layout count "
                    << module.record_layouts.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadRecordLayoutIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto layout_index = static_cast<std::size_t>(ins.operand);
        const auto& layout = module.record_layouts[layout_index];
        if (state.stack.size() < layout.field_types.size()) {
            std::ostringstream message;
            message << "record initializer requires "
                    << layout.field_types.size() << " fields but stack has "
                    << state.stack.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadRecordInitializerArity,
                          instruction_message(function, pc, message.str()));
        }
        for (std::size_t i = layout.field_types.size(); i > 0; --i) {
            const auto field_index = i - 1;
            AbstractValue initializer;
            std::ostringstream context;
            context << "record initializer field " << field_index;
            if (!pop_any_or_report(
                    state, diagnostics, function, function_index, pc,
                    VerifierReason::BadRecordInitializerArity,
                    context.str(), &initializer)) {
                return false;
            }
            if (!value_conforms_to_signature(
                    module, state, initializer,
                    layout.field_types[field_index])) {
                std::ostringstream message;
                message << "record initializer field " << field_index
                        << " does not conform to its declared type";
                return reject(diagnostics, function_index, pc,
                              VerifierReason::BadRecordInitializerType,
                              instruction_message(function, pc,
                                                  message.str()));
            }
        }
        state.stack.push_back(record_value(layout_index, false));
        return push_fallthrough_or_report(
            pc, function, function_index, std::move(state), successors,
            diagnostics);
    }
    case OpCode::RecordGet:
    case OpCode::RecordSet: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.record_layouts.size()) {
            std::ostringstream message;
            message << "record layout " << ins.operand
                    << " is outside layout count "
                    << module.record_layouts.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadRecordLayoutIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto layout_index = static_cast<std::size_t>(ins.operand);
        const auto& layout = module.record_layouts[layout_index];
        if (ins.operand2 < 0 ||
            static_cast<std::uint64_t>(ins.operand2) >=
                layout.field_types.size()) {
            std::ostringstream message;
            message << "record field " << ins.operand2
                    << " is outside field count "
                    << layout.field_types.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadRecordFieldIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto field_index = static_cast<std::size_t>(ins.operand2);
        AbstractValue stored;
        if (ins.op == OpCode::RecordSet &&
            !pop_any_or_report(state, diagnostics, function, function_index,
                               pc, VerifierReason::StackUnderflow,
                               "record stored value", &stored)) {
            return false;
        }
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index,
                               pc, VerifierReason::StackUnderflow,
                               "record receiver", &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Record) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::RecordOperationOnNonRecord,
                          instruction_message(
                              function, pc,
                              "receiver is not a typed record reference"));
        }
        if (receiver.includes_nil) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::RecordReceiverMayBeNil,
                          instruction_message(
                              function, pc,
                              "record receiver may be nil without refinement"));
        }
        if (!receiver.signature_record_layout.has_value() ||
            *receiver.signature_record_layout != layout_index) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::RecordLayoutMismatch,
                          instruction_message(
                              function, pc,
                              "receiver nominal layout differs from opcode layout"));
        }
        if (ins.op == OpCode::RecordSet) {
            if (!value_conforms_to_signature(
                    module, state, stored, layout.field_types[field_index])) {
                return reject(diagnostics, function_index, pc,
                              VerifierReason::RecordFieldTypeMismatch,
                              instruction_message(
                                  function, pc,
                                  "stored value violates record field type"));
            }
        } else {
            state.stack.push_back(
                value_from_signature(layout.field_types[field_index]));
        }
        return push_fallthrough_or_report(
            pc, function, function_index, std::move(state), successors,
            diagnostics);
    }
    case OpCode::AllocVariant: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.variant_layouts.size()) {
            std::ostringstream message;
            message << "variant layout " << ins.operand
                    << " is outside layout count "
                    << module.variant_layouts.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadVariantLayoutIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto layout_index = static_cast<std::size_t>(ins.operand);
        const auto& layout = module.variant_layouts[layout_index];
        if (ins.operand2 < 0 ||
            static_cast<std::uint64_t>(ins.operand2) >=
                layout.cases.size()) {
            std::ostringstream message;
            message << "variant case " << ins.operand2
                    << " is outside case count " << layout.cases.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadVariantCaseIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto case_index = static_cast<std::size_t>(ins.operand2);
        const auto& case_layout = layout.cases[case_index];
        if (state.stack.size() < case_layout.field_types.size()) {
            std::ostringstream message;
            message << "variant initializer requires "
                    << case_layout.field_types.size()
                    << " fields but stack has " << state.stack.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadVariantInitializerArity,
                          instruction_message(function, pc, message.str()));
        }
        for (std::size_t i = case_layout.field_types.size(); i > 0; --i) {
            const auto field_index = i - 1;
            AbstractValue initializer;
            std::ostringstream context;
            context << "variant initializer field " << field_index;
            if (!pop_any_or_report(
                    state, diagnostics, function, function_index, pc,
                    VerifierReason::BadVariantInitializerArity,
                    context.str(), &initializer)) {
                return false;
            }
            if (!value_conforms_to_signature(
                    module, state, initializer,
                    case_layout.field_types[field_index])) {
                std::ostringstream message;
                message << "variant initializer field " << field_index
                        << " does not conform to its declared type";
                return reject(diagnostics, function_index, pc,
                              VerifierReason::BadVariantInitializerType,
                              instruction_message(function, pc,
                                                  message.str()));
            }
        }
        state.stack.push_back(variant_value(layout_index, false));
        return push_fallthrough_or_report(
            pc, function, function_index, std::move(state), successors,
            diagnostics);
    }
    case OpCode::VariantTag: {
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index,
                               pc, VerifierReason::StackUnderflow,
                               "variant receiver", &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Variant) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::VariantOperationOnNonVariant,
                          instruction_message(
                              function, pc,
                              "receiver is not a typed variant reference"));
        }
        if (receiver.includes_nil) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::VariantReceiverMayBeNil,
                          instruction_message(
                              function, pc,
                              "variant receiver may be nil without refinement"));
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(
            pc, function, function_index, std::move(state), successors,
            diagnostics);
    }
    case OpCode::VariantGet: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.variant_layouts.size()) {
            std::ostringstream message;
            message << "variant layout " << ins.operand
                    << " is outside layout count "
                    << module.variant_layouts.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadVariantLayoutIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto layout_index = static_cast<std::size_t>(ins.operand);
        const auto& layout = module.variant_layouts[layout_index];
        if (ins.operand2 < 0 ||
            static_cast<std::uint64_t>(ins.operand2) >=
                layout.cases.size()) {
            std::ostringstream message;
            message << "variant case " << ins.operand2
                    << " is outside case count " << layout.cases.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadVariantCaseIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto case_index = static_cast<std::size_t>(ins.operand2);
        const auto& case_layout = layout.cases[case_index];
        if (ins.operand3 < 0 ||
            static_cast<std::uint64_t>(ins.operand3) >=
                case_layout.field_types.size()) {
            std::ostringstream message;
            message << "variant field " << ins.operand3
                    << " is outside field count "
                    << case_layout.field_types.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadVariantFieldIndex,
                          instruction_message(function, pc, message.str()));
        }
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index,
                               pc, VerifierReason::StackUnderflow,
                               "variant receiver", &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Variant) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::VariantOperationOnNonVariant,
                          instruction_message(
                              function, pc,
                              "receiver is not a typed variant reference"));
        }
        if (receiver.includes_nil) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::VariantReceiverMayBeNil,
                          instruction_message(
                              function, pc,
                              "variant receiver may be nil without refinement"));
        }
        if (!receiver.signature_variant_layout.has_value() ||
            *receiver.signature_variant_layout != layout_index) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::VariantLayoutMismatch,
                          instruction_message(
                              function, pc,
                              "receiver nominal layout differs from opcode layout"));
        }
        const auto field_index = static_cast<std::size_t>(ins.operand3);
        state.stack.push_back(
            value_from_signature(case_layout.field_types[field_index]));
        return push_fallthrough_or_report(
            pc, function, function_index, std::move(state), successors,
            diagnostics);
    }
    case OpCode::TryBegin:
        if (ins.operand < 0 || static_cast<std::size_t>(ins.operand) >=
                                   function.exception_handlers.size() ||
            function.exception_handlers[static_cast<std::size_t>(ins.operand)].try_begin != pc) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadExceptionHandler,
                          instruction_message(function, pc, "invalid try-begin delimiter"));
        }
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors, diagnostics);
    case OpCode::TryEnd:
        if (ins.operand < 0 || static_cast<std::size_t>(ins.operand) >=
                                   function.exception_handlers.size() ||
            function.exception_handlers[static_cast<std::size_t>(ins.operand)].try_end != pc) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadExceptionHandler,
                          instruction_message(function, pc, "invalid try-end delimiter"));
        }
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors, diagnostics);
    case OpCode::Throw: {
        AbstractValue thrown;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "throw operand", &thrown)) {
            return false;
        }
        if (thrown.kind != AbstractKind::Variant || thrown.includes_nil ||
            !thrown.signature_variant_layout.has_value()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::ThrowRequiresVariant,
                          instruction_message(function, pc,
                                              "throw requires non-nil nominal variant"));
        }
        if (const auto* handler = enclosing_handler(
                function, pc, thrown.signature_variant_layout)) {
            add_exception_successor(*handler, std::move(state), successors);
        }
        return true;
    }
    case OpCode::AllocArray:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array init value") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array length")) {
            return false;
        }
        state.stack.push_back(array_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::ArrayGet:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Array, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array receiver")) {
            return false;
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::ArraySet:
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array stored value") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Array, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation, "array receiver")) {
            return false;
        }
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::ArrayLen:
    {
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "array receiver",
                               &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Array &&
            receiver.kind != AbstractKind::RefArray) {
            std::ostringstream message;
            message << "array receiver expected Array or RefArray but found "
                    << abstract_kind_name(receiver.kind);
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadArrayOperation,
                          instruction_message(function, pc, message.str()));
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::AllocRefArray:
    {
        AbstractValue init;
        if (!pop_reference_or_report(state, diagnostics, function, function_index, pc,
                                     VerifierReason::StackUnderflow,
                                     VerifierReason::BadArrayOperation,
                                     "ref array init value", &init) ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array length")) {
            return false;
        }
        state.ref_array_elements_by_site[pc] = without_provenance(init);
        state.stack.push_back(ref_array_value(pc));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::RefArrayGet:
    {
        AbstractValue receiver;
        AbstractValue loaded;
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::RefArray, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array receiver", &receiver)) {
            return false;
        }
        if (!load_ref_array_element(state, receiver, loaded)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadArrayOperation,
                          instruction_message(function, pc,
                                              "ref array element facts are unavailable or incompatible"));
        }
        state.stack.push_back(std::move(loaded));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::RefArraySet:
    {
        AbstractValue value;
        AbstractValue receiver;
        if (!pop_reference_or_report(state, diagnostics, function, function_index, pc,
                                     VerifierReason::StackUnderflow,
                                     VerifierReason::BadArrayOperation,
                                     "ref array stored value", &value) ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array index") ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::RefArray, VerifierReason::StackUnderflow,
                                  VerifierReason::BadArrayOperation,
                                  "ref array receiver", &receiver)) {
            return false;
        }
        if (!store_ref_array_element(module, state, receiver,
                                     without_provenance(value))) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadArrayOperation,
                          instruction_message(function, pc,
                                              "stored value does not satisfy ref array element facts"));
        }
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::AllocMap: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.map_layouts.size()) {
            std::ostringstream message;
            message << "map layout operand " << ins.operand
                    << " is outside layout count " << module.map_layouts.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadMapLayoutIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto& layout =
            module.map_layouts[static_cast<std::size_t>(ins.operand)];
        state.stack.push_back(map_value(map_signature(layout.key_type,
                                                      layout.value_type)));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::MapGet:
    case OpCode::MapHas: {
        AbstractValue key;
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map key", &key) ||
            !pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map receiver",
                               &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Map ||
            receiver.signature_map == nullptr ||
            !receiver.signature_map->has_map_entries()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapOperationOnNonMap,
                          instruction_message(function, pc,
                                              "receiver is not a typed map"));
        }
        if (!value_conforms_to_signature(module, state, key,
                                         *receiver.signature_map->key)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapKeyTypeMismatch,
                          instruction_message(function, pc,
                                              "key does not match map key type"));
        }
        state.stack.push_back(
            ins.op == OpCode::MapHas
                ? bool_value()
                : value_from_signature(*receiver.signature_map->value));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::MapSet: {
        AbstractValue stored;
        AbstractValue key;
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow,
                               "map stored value", &stored) ||
            !pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map key", &key) ||
            !pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map receiver",
                               &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Map ||
            receiver.signature_map == nullptr ||
            !receiver.signature_map->has_map_entries()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapOperationOnNonMap,
                          instruction_message(function, pc,
                                              "receiver is not a typed map"));
        }
        if (!value_conforms_to_signature(module, state, key,
                                         *receiver.signature_map->key)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapKeyTypeMismatch,
                          instruction_message(function, pc,
                                              "key does not match map key type"));
        }
        if (!value_conforms_to_signature(module, state, stored,
                                         *receiver.signature_map->value)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapValueTypeMismatch,
                          instruction_message(function, pc,
                                              "value does not match map value type"));
        }
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::MapLen: {
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "map receiver",
                               &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Map ||
            receiver.signature_map == nullptr ||
            !receiver.signature_map->has_map_entries()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::MapOperationOnNonMap,
                          instruction_message(function, pc,
                                              "receiver is not a typed map"));
        }
        state.stack.push_back(int64_value());
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::MapKeyAt:
    case OpCode::MapValueAt: {
        AbstractValue receiver;
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Int64,
                                  VerifierReason::StackUnderflow,
                                  VerifierReason::BadMapPositionAccess,
                                  "map positional index") ||
            !pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow,
                               "map positional receiver", &receiver)) {
            return false;
        }
        if (receiver.kind != AbstractKind::Map ||
            receiver.signature_map == nullptr ||
            !receiver.signature_map->has_map_entries()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadMapPositionAccess,
                          instruction_message(function, pc,
                                              "receiver is not a typed map"));
        }
        state.stack.push_back(value_from_signature(
            ins.op == OpCode::MapKeyAt ? *receiver.signature_map->key
                                       : *receiver.signature_map->value));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::GetLeft:
    case OpCode::GetRight: {
        AbstractValue receiver;
        AbstractValue loaded;
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Object, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "pair receiver",
                                  &receiver)) {
            return false;
        }
        if (!load_pair_field(module, state, receiver, ins.op == OpCode::GetLeft,
                             loaded)) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadPairFieldRead,
                          instruction_message(function, pc,
                                              "pair field facts are unavailable or incompatible"));
        }
        if (is_poison(loaded)) {
            return reject(diagnostics, function_index, pc, VerifierReason::PoisonUse,
                          instruction_message(function, pc,
                                              "pair field read produced a poison value"));
        }
        state.stack.push_back(std::move(loaded));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::SetLeft:
    case OpCode::SetRight: {
        AbstractValue value;
        AbstractValue receiver;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "stored field value",
                               &value) ||
            !pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Object, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "pair receiver",
                                  &receiver)) {
            return false;
        }
        if (!store_pair_field(module, state, receiver, ins.op == OpCode::SetLeft,
                              without_provenance(value))) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadPairFieldWrite,
                          instruction_message(function, pc,
                                              "stored value does not satisfy pair field facts"));
        }
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::LoadLocal: {
        if (!is_valid_local_index(ins.operand, function.local_count)) {
            std::ostringstream message;
            message << "local operand " << ins.operand
                    << " is outside local_count " << function.local_count;
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadLocalIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto local_index = static_cast<std::size_t>(ins.operand);
        if (!state.locals[local_index].has_value() || is_poison(*state.locals[local_index])) {
            if (!state.locals[local_index].has_value()) {
                std::ostringstream message;
                message << "local " << local_index
                        << " is uninitialized on at least one incoming path";
                return reject(diagnostics, function_index, pc,
                              VerifierReason::UninitializedLocal,
                              instruction_message(function, pc, message.str()));
            }
            std::ostringstream message;
            message << "local " << local_index << " contains a poison value";
            return reject(diagnostics, function_index, pc, VerifierReason::PoisonUse,
                          instruction_message(function, pc, message.str()));
        }
        auto loaded = *state.locals[local_index];
        loaded.source_local = local_index;
        loaded.nil_test_local.reset();
        state.stack.push_back(std::move(loaded));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::StoreLocal: {
        if (!is_valid_local_index(ins.operand, function.local_count)) {
            std::ostringstream message;
            message << "local operand " << ins.operand
                    << " is outside local_count " << function.local_count;
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadLocalIndex,
                          instruction_message(function, pc, message.str()));
        }
        AbstractValue stored;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "stored local value",
                               &stored)) {
            return false;
        }
        state.locals[static_cast<std::size_t>(ins.operand)] =
            without_provenance(stored);
        state.local_reference_kinds[static_cast<std::size_t>(ins.operand)] =
            is_reference_kind(stored.kind);
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::Jump:
        if (!is_valid_target(ins.operand, function.code.size())) {
            std::ostringstream message;
            message << "target " << ins.operand << " is outside code size "
                    << function.code.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadJumpTarget,
                          instruction_message(function, pc, message.str()));
        }
        successors.emplace_back(static_cast<std::size_t>(ins.operand), std::move(state));
        return true;
    case OpCode::JumpIfFalse: {
        if (!is_valid_target(ins.operand, function.code.size())) {
            std::ostringstream message;
            message << "target " << ins.operand << " is outside code size "
                    << function.code.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadJumpTarget,
                          instruction_message(function, pc, message.str()));
        }
        AbstractValue condition;
        if (!pop_expect_or_report(state, diagnostics, function, function_index, pc,
                                  AbstractKind::Bool, VerifierReason::StackUnderflow,
                                  VerifierReason::TypeMismatch, "branch condition",
                                  &condition)) {
            return false;
        }
        auto false_state = state;
        auto true_state = state;
        if (condition.nil_test_local.has_value()) {
            const auto local = *condition.nil_test_local;
            if (local < true_state.locals.size() && true_state.locals[local].has_value()) {
                if (true_state.locals[local]->kind !=
                    AbstractKind::MaybeReference) {
                    true_state.locals[local] = nil_object_value();
                }
            }
            if (local < false_state.locals.size() &&
                false_state.locals[local].has_value()) {
                if (false_state.locals[local]->kind ==
                        AbstractKind::MaybeReference &&
                    false_state.locals[local]->reference_target != nullptr) {
                    false_state.locals[local] = without_provenance(
                        *false_state.locals[local]->reference_target);
                } else {
                    false_state.locals[local]->includes_nil = false;
                }
            }
        }
        successors.emplace_back(static_cast<std::size_t>(ins.operand),
                                std::move(false_state));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(true_state),
                                          successors, diagnostics);
    }
    case OpCode::Collect:
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    case OpCode::AllocClosure: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                module.closure_layouts.size()) {
            std::ostringstream message;
            message << "layout " << ins.operand << " is outside closure layout count "
                    << module.closure_layouts.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadClosureLayoutIndex,
                          instruction_message(function, pc, message.str()));
        }
        const auto& layout =
            module.closure_layouts[static_cast<std::size_t>(ins.operand)];
        for (std::size_t i = layout.capture_types.size(); i > 0; --i) {
            AbstractValue capture;
            const auto capture_index = i - 1;
            std::ostringstream context;
            context << "capture " << capture_index;
            if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                                   VerifierReason::BadClosureCaptureArity,
                                   context.str(), &capture)) {
                return false;
            }
            if (!value_conforms_to_signature(module, state, capture,
                                             layout.capture_types[capture_index])) {
                std::ostringstream message;
                message << "capture " << capture_index
                        << " does not conform to closure layout type "
                        << value_kind_name(
                               layout.capture_types[capture_index].kind);
                return reject(diagnostics, function_index, pc,
                              VerifierReason::BadClosureCaptureType,
                              instruction_message(function, pc, message.str()));
            }
        }
        state.stack.push_back(value_from_signature(layout.function_type));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::CallClosure: {
        AbstractValue callee;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::BadClosureCallArity,
                               "closure callee", &callee)) {
            return false;
        }
        if (callee.kind != AbstractKind::Function ||
            callee.signature_function == nullptr ||
            !callee.signature_function->has_function_signature()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::CallClosureOnNonFunction,
                          instruction_message(
                              function, pc,
                              "callee stack slot is not a structural function"));
        }
        const auto& signature = *callee.signature_function;
        for (std::size_t i = signature.function_parameters.size(); i > 0; --i) {
            AbstractValue argument;
            const auto parameter_index = i - 1;
            std::ostringstream context;
            context << "closure argument " << parameter_index;
            if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                                   VerifierReason::BadClosureCallArity,
                                   context.str(), &argument)) {
                return false;
            }
            if (!value_conforms_to_signature(
                    module, state, argument,
                    signature.function_parameters[parameter_index])) {
                std::ostringstream message;
                message << "closure argument " << parameter_index
                        << " does not conform to parameter type "
                        << value_kind_name(
                               signature.function_parameters[parameter_index].kind);
                return reject(diagnostics, function_index, pc,
                              VerifierReason::BadClosureCallArgKind,
                              instruction_message(function, pc, message.str()));
            }
        }
        state.stack.push_back(value_from_signature(*signature.function_return));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::LoadCapture: {
        if (!function.closure_layout.has_value()) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::LoadCaptureOutsideClosureBody,
                          instruction_message(
                              function, pc,
                              "current function has no closure-body layout"));
        }
        assert(*function.closure_layout < module.closure_layouts.size() &&
               "module closure layouts are validated before function analysis");
        const auto& layout = module.closure_layouts[*function.closure_layout];
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >=
                layout.capture_types.size()) {
            std::ostringstream message;
            message << "capture " << ins.operand << " is outside capture count "
                    << layout.capture_types.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::LoadCaptureOutOfRange,
                          instruction_message(function, pc, message.str()));
        }
        state.stack.push_back(value_from_signature(
            layout.capture_types[static_cast<std::size_t>(ins.operand)]));
        return push_fallthrough_or_report(pc, function, function_index,
                                          std::move(state), successors,
                                          diagnostics);
    }
    case OpCode::Call: {
        if (ins.operand < 0 ||
            static_cast<std::uint64_t>(ins.operand) >= module.functions.size()) {
            std::ostringstream message;
            message << "callee " << ins.operand << " is outside function count "
                    << module.functions.size();
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadCallTarget,
                          instruction_message(function, pc, message.str()));
        }
        const auto callee_index = static_cast<std::size_t>(ins.operand);
        const auto& callee_function = module.functions[callee_index];
        if (callee_function.closure_layout.has_value()) {
            assert(*callee_function.closure_layout <
                       module.closure_layouts.size() &&
                   "closure-body layout is validated before transfer");
            if (!module.closure_layouts[*callee_function.closure_layout]
                     .capture_types.empty()) {
                return reject(
                    diagnostics, function_index, pc,
                    VerifierReason::BadCallTarget,
                    instruction_message(
                        function, pc,
                        "direct Call cannot target a capture-bearing closure body"));
            }
        }
        const auto& callee = callee_function.signature;
        for (std::size_t i = callee.parameters.size(); i > 0; --i) {
            AbstractValue argument;
            const auto parameter_index = i - 1;
            std::ostringstream context;
            context << "argument " << parameter_index;
            if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                                   VerifierReason::BadCallArity, context.str(),
                                   &argument)) {
                return false;
            }
            if (!value_conforms_to_signature(module, state, argument,
                                             parameter_signature(callee,
                                                                 parameter_index))) {
                std::ostringstream message;
                message << "argument " << parameter_index
                        << " does not conform to callee parameter "
                        << value_kind_name(callee.parameters[parameter_index]);
                return reject(diagnostics, function_index, pc,
                              VerifierReason::BadCallArgKind,
                              instruction_message(function, pc, message.str()));
            }
        }
        state.stack.push_back(value_from_signature(return_signature(callee)));
        return push_fallthrough_or_report(pc, function, function_index, std::move(state),
                                          successors, diagnostics);
    }
    case OpCode::Return: {
        AbstractValue returned;
        if (!pop_any_or_report(state, diagnostics, function, function_index, pc,
                               VerifierReason::StackUnderflow, "return value",
                               &returned)) {
            return false;
        }
        if (!value_conforms_to_signature(module, state, returned,
                                         return_signature(function.signature))) {
            return reject(diagnostics, function_index, pc,
                          VerifierReason::BadReturnKind,
                          instruction_message(function, pc,
                                              "return value does not conform to function signature"));
        }
        return true;
    }
    }
    return reject(diagnostics, function_index, pc, VerifierReason::InvalidOpcode,
                  instruction_message(function, pc, "unknown opcode"));
}

std::optional<VerificationResult> verify_function_with_stack_maps(
    const Module& module, std::size_t function_index,
    std::vector<VerifierDiagnostic>& diagnostics) {
    const auto& function = module.functions[function_index];
    if (!function.stack_maps.empty() && function.stack_maps.size() != function.code.size()) {
        std::ostringstream message;
        message << "stack_maps size " << function.stack_maps.size()
                << " does not match code size " << function.code.size();
        reject(diagnostics, function_index, std::nullopt, VerifierReason::BadStackMap,
               message.str());
        return std::nullopt;
    }
    if (function.code.empty()) {
        reject(diagnostics, function_index, std::nullopt, VerifierReason::EmptyFunction,
               "function has no bytecode");
        return std::nullopt;
    }
    if (!signature_is_well_formed(function.signature, module)) {
        reject(diagnostics, function_index, std::nullopt,
               VerifierReason::SignatureShapeMismatch,
               "function signature detail shape does not match coarse kinds");
        return std::nullopt;
    }
    if (function.local_count < function.signature.parameters.size()) {
        std::ostringstream message;
        message << "local_count " << function.local_count
                << " is smaller than parameter count "
                << function.signature.parameters.size();
        reject(diagnostics, function_index, std::nullopt,
               VerifierReason::LocalCountMismatch, message.str());
        return std::nullopt;
    }
    for (std::size_t i = 0; i < function.exception_handlers.size(); ++i) {
        const auto& handler = function.exception_handlers[i];
        const bool bounds = handler.try_begin < handler.try_end &&
                            handler.try_end < function.code.size() &&
                            handler.target < function.code.size() &&
                            handler.target > handler.try_end &&
                            handler.variant_layout < module.variant_layouts.size();
        const bool delimiters = bounds &&
            function.code[handler.try_begin].op == OpCode::TryBegin &&
            function.code[handler.try_begin].operand == static_cast<std::int64_t>(i) &&
            function.code[handler.try_end].op == OpCode::TryEnd &&
            function.code[handler.try_end].operand == static_cast<std::int64_t>(i);
        if (!delimiters) {
            reject(diagnostics, function_index, std::nullopt,
                   VerifierReason::BadExceptionHandler,
                   "exception handler range, target, layout, or delimiters are malformed");
            return std::nullopt;
        }
        for (std::size_t j = 0; j < i; ++j) {
            const auto& other = function.exception_handlers[j];
            const bool crossing = other.try_begin < handler.try_begin &&
                                  handler.try_begin < other.try_end &&
                                  other.try_end < handler.try_end;
            if (crossing || other.target == handler.target) {
                reject(diagnostics, function_index, std::nullopt,
                       VerifierReason::BadExceptionHandler,
                       "exception handlers must be properly nested with distinct targets");
                return std::nullopt;
            }
        }
    }

    std::vector<std::optional<AbstractState>> states(function.code.size());
    std::deque<std::size_t> worklist;

    AbstractState initial;
    initial.locals.resize(function.local_count);
    initial.local_reference_kinds.resize(function.local_count);
    for (std::size_t i = 0; i < function.signature.parameters.size(); ++i) {
        initial.locals[i] = value_from_signature(parameter_signature(function.signature, i));
        initial.local_reference_kinds[i] =
            is_reference_kind(initial.locals[i]->kind);
    }
    states[0] = initial;
    worklist.push_back(0);

    std::vector<std::pair<std::size_t, AbstractState>> successors;
    successors.reserve(2);
    while (!worklist.empty()) {
        const auto pc = worklist.front();
        worklist.pop_front();

        successors.clear();
        if (!transfer_instruction(module, function_index, pc, *states[pc], successors,
                                  diagnostics)) {
            return std::nullopt;
        }
        const auto op = function.code[pc].op;
        if (op == OpCode::TryBegin) {
            const auto index = static_cast<std::size_t>(function.code[pc].operand);
            add_exception_successor(function.exception_handlers[index],
                                    *states[pc], successors);
        }
        if (op == OpCode::Call || op == OpCode::CallClosure) {
            if (const auto* handler = enclosing_handler(function, pc)) {
                add_exception_successor(*handler, *states[pc], successors);
            }
        }

        for (auto& [target, state] : successors) {
            if (!states[target].has_value()) {
                states[target] = std::move(state);
                worklist.push_back(target);
                continue;
            }

            if (states[target]->stack.size() != state.stack.size()) {
                std::ostringstream message;
                message << "target pc " << target << " has incoming stack height "
                        << state.stack.size() << " but existing height is "
                        << states[target]->stack.size();
                reject(diagnostics, function_index, target,
                       VerifierReason::StackHeightMergeMismatch, message.str());
                return std::nullopt;
            }
            if (states[target]->locals.size() != state.locals.size()) {
                std::ostringstream message;
                message << "target pc " << target << " has incoming local count "
                        << state.locals.size() << " but existing count is "
                        << states[target]->locals.size();
                reject(diagnostics, function_index, target,
                       VerifierReason::LocalCountMismatch, message.str());
                return std::nullopt;
            }
            const auto outcome = join_state_into(*states[target], state);
            if (outcome == JoinOutcome::Invalid) {
                reject(diagnostics, function_index, target,
                       VerifierReason::StackHeightMergeMismatch,
                       "control-flow merge has incompatible state shape");
                return std::nullopt;
            }
            if (outcome == JoinOutcome::Changed) {
                worklist.push_back(target);
            }
        }
    }

    VerificationResult result;
    result.stack_maps.resize(function.code.size());
    for (std::size_t pc = 0; pc < function.code.size(); ++pc) {
        if (!states[pc].has_value()) {
            reject(diagnostics, function_index, pc, VerifierReason::UnreachableCode,
                   "bytecode pc is unreachable from function entry");
            return std::nullopt;
        }
        auto map = stack_map_from_state(*states[pc]);
        if (!map.has_value()) {
            reject(diagnostics, function_index, pc, VerifierReason::PoisonUse,
                   "cannot generate stack map for poison stack value");
            return std::nullopt;
        }
        if (!function.stack_maps.empty() &&
            !stack_map_matches(function.stack_maps[pc], *states[pc])) {
            reject(diagnostics, function_index, pc, VerifierReason::BadStackMap,
                   "supplied stack map does not match verifier state");
            return std::nullopt;
        }
        result.stack_maps[pc] = *map;
    }

    return result;
}

} // namespace

const char* verifier_reason_name(VerifierReason reason) {
    switch (reason) {
    case VerifierReason::ModuleShapeMismatch:
        return "ModuleShapeMismatch";
    case VerifierReason::EmptyFunction:
        return "EmptyFunction";
    case VerifierReason::SignatureShapeMismatch:
        return "SignatureShapeMismatch";
    case VerifierReason::LocalCountMismatch:
        return "LocalCountMismatch";
    case VerifierReason::BadStackMap:
        return "BadStackMap";
    case VerifierReason::StackUnderflow:
        return "StackUnderflow";
    case VerifierReason::TypeMismatch:
        return "TypeMismatch";
    case VerifierReason::PoisonUse:
        return "PoisonUse";
    case VerifierReason::UninitializedLocal:
        return "UninitializedLocal";
    case VerifierReason::BadLocalIndex:
        return "BadLocalIndex";
    case VerifierReason::BadJumpTarget:
        return "BadJumpTarget";
    case VerifierReason::FallOffEnd:
        return "FallOffEnd";
    case VerifierReason::StackHeightMergeMismatch:
        return "StackHeightMergeMismatch";
    case VerifierReason::UnreachableCode:
        return "UnreachableCode";
    case VerifierReason::BadPairFieldRead:
        return "BadPairFieldRead";
    case VerifierReason::BadPairFieldWrite:
        return "BadPairFieldWrite";
    case VerifierReason::BadArrayOperation:
        return "BadArrayOperation";
    case VerifierReason::BadCallTarget:
        return "BadCallTarget";
    case VerifierReason::BadCallArity:
        return "BadCallArity";
    case VerifierReason::BadCallArgKind:
        return "BadCallArgKind";
    case VerifierReason::BadReturnKind:
        return "BadReturnKind";
    case VerifierReason::InvalidOpcode:
        return "InvalidOpcode";
    case VerifierReason::BadStringConstantIndex:
        return "BadStringConstantIndex";
    case VerifierReason::BadStringOperation:
        return "BadStringOperation";
    case VerifierReason::BadClosureLayoutIndex:
        return "BadClosureLayoutIndex";
    case VerifierReason::BadClosureCaptureArity:
        return "BadClosureCaptureArity";
    case VerifierReason::BadClosureCaptureType:
        return "BadClosureCaptureType";
    case VerifierReason::CallClosureOnNonFunction:
        return "CallClosureOnNonFunction";
    case VerifierReason::BadClosureCallArity:
        return "BadClosureCallArity";
    case VerifierReason::BadClosureCallArgKind:
        return "BadClosureCallArgKind";
    case VerifierReason::LoadCaptureOutOfRange:
        return "LoadCaptureOutOfRange";
    case VerifierReason::LoadCaptureOutsideClosureBody:
        return "LoadCaptureOutsideClosureBody";
    case VerifierReason::BadMapLayoutIndex:
        return "BadMapLayoutIndex";
    case VerifierReason::InvalidMapKeyType:
        return "InvalidMapKeyType";
    case VerifierReason::MapOperationOnNonMap:
        return "MapOperationOnNonMap";
    case VerifierReason::MapKeyTypeMismatch:
        return "MapKeyTypeMismatch";
    case VerifierReason::MapValueTypeMismatch:
        return "MapValueTypeMismatch";
    case VerifierReason::BadWeakTargetType:
        return "BadWeakTargetType";
    case VerifierReason::WeakOperationOnNonWeak:
        return "WeakOperationOnNonWeak";
    case VerifierReason::WeakTargetMayBeNil:
        return "WeakTargetMayBeNil";
    case VerifierReason::BadMapPositionAccess:
        return "BadMapPositionAccess";
    case VerifierReason::PrintRequiresStr:
        return "PrintRequiresStr";
    case VerifierReason::I64ToStrRequiresI64:
        return "I64ToStrRequiresI64";
    case VerifierReason::StrToI64RequiresStr:
        return "StrToI64RequiresStr";
    case VerifierReason::BoolToStrRequiresBool:
        return "BoolToStrRequiresBool";
    case VerifierReason::StrSubRequiresStr:
        return "StrSubRequiresStr";
    case VerifierReason::StrSubRequiresI64Bounds:
        return "StrSubRequiresI64Bounds";
    case VerifierReason::StrLtRequiresStr:
        return "StrLtRequiresStr";
    case VerifierReason::BadRecordLayoutShape:
        return "BadRecordLayoutShape";
    case VerifierReason::BadRecordLayoutIndex:
        return "BadRecordLayoutIndex";
    case VerifierReason::BadRecordFieldIndex:
        return "BadRecordFieldIndex";
    case VerifierReason::BadRecordInitializerArity:
        return "BadRecordInitializerArity";
    case VerifierReason::BadRecordInitializerType:
        return "BadRecordInitializerType";
    case VerifierReason::RecordOperationOnNonRecord:
        return "RecordOperationOnNonRecord";
    case VerifierReason::RecordLayoutMismatch:
        return "RecordLayoutMismatch";
    case VerifierReason::RecordReceiverMayBeNil:
        return "RecordReceiverMayBeNil";
    case VerifierReason::RecordFieldTypeMismatch:
        return "RecordFieldTypeMismatch";
    case VerifierReason::BadVariantLayoutShape:
        return "BadVariantLayoutShape";
    case VerifierReason::BadVariantLayoutIndex:
        return "BadVariantLayoutIndex";
    case VerifierReason::BadVariantCaseIndex:
        return "BadVariantCaseIndex";
    case VerifierReason::BadVariantFieldIndex:
        return "BadVariantFieldIndex";
    case VerifierReason::BadVariantInitializerArity:
        return "BadVariantInitializerArity";
    case VerifierReason::BadVariantInitializerType:
        return "BadVariantInitializerType";
    case VerifierReason::VariantOperationOnNonVariant:
        return "VariantOperationOnNonVariant";
    case VerifierReason::VariantLayoutMismatch:
        return "VariantLayoutMismatch";
    case VerifierReason::VariantReceiverMayBeNil:
        return "VariantReceiverMayBeNil";
    case VerifierReason::BadExceptionHandler:
        return "BadExceptionHandler";
    case VerifierReason::ThrowRequiresVariant:
        return "ThrowRequiresVariant";
    }
    return "<unknown>";
}

std::string format_verifier_diagnostic(const VerifierDiagnostic& diagnostic) {
    std::ostringstream out;
    out << "function=" << diagnostic.function_index << " ";
    if (diagnostic.pc.has_value()) {
        out << "pc=" << *diagnostic.pc;
    } else {
        out << "pc=<none>";
    }
    out << " reason=" << verifier_reason_name(diagnostic.reason);
    if (!diagnostic.message.empty()) {
        out << ": " << diagnostic.message;
    }
    return out.str();
}

FunctionVerifierReport verify_with_diagnostics(const Function& function) {
    Module module;
    module.entry_function = 0;
    module.functions.push_back(function);

    auto module_report = verify_with_diagnostics(module);
    FunctionVerifierReport report;
    if (module_report.result.has_value() && !module_report.result->functions.empty()) {
        report.result = module_report.result->functions.front();
    }
    report.diagnostics = std::move(module_report.diagnostics);
    return report;
}

ModuleVerifierReport verify_with_diagnostics(const Module& module) {
    ModuleVerifierReport report;
    if (module.functions.empty()) {
        reject(report.diagnostics, 0, std::nullopt,
               VerifierReason::ModuleShapeMismatch,
               "module has no functions");
        return report;
    }
    if (module.entry_function >= module.functions.size()) {
        std::ostringstream message;
        message << "entry function " << module.entry_function
                << " is outside function count " << module.functions.size();
        reject(report.diagnostics, module.entry_function, std::nullopt,
               VerifierReason::ModuleShapeMismatch, message.str());
        return report;
    }
    if (!record_layouts_are_well_formed(module, report.diagnostics)) {
        return report;
    }
    if (!variant_layouts_are_well_formed(module, report.diagnostics)) {
        return report;
    }
    if (!module_weak_target_types_are_valid(module, report.diagnostics)) {
        return report;
    }
    if (!module_map_key_types_are_valid(module, report.diagnostics)) {
        return report;
    }
    if (!map_layouts_are_well_formed(module, report.diagnostics)) {
        return report;
    }
    if (!named_types_are_well_formed(module)) {
        reject(report.diagnostics, 0, std::nullopt,
               VerifierReason::SignatureShapeMismatch,
               "module named type table contains a malformed recursive type");
        return report;
    }
    if (!closure_layouts_are_well_formed(module, report.diagnostics)) {
        return report;
    }
    const auto& entry = module.functions[module.entry_function];
    if (entry.closure_layout.has_value() &&
        !module.closure_layouts[*entry.closure_layout].capture_types.empty()) {
        reject(report.diagnostics, module.entry_function, std::nullopt,
               VerifierReason::BadCallTarget,
               "module entry cannot be a capture-bearing closure body");
        return report;
    }

    ModuleVerificationResult result;
    result.functions.reserve(module.functions.size());
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        auto function_result = verify_function_with_stack_maps(module, i,
                                                               report.diagnostics);
        if (!function_result.has_value()) {
            return report;
        }
        result.functions.push_back(std::move(*function_result));
    }
    report.result = std::move(result);
    return report;
}

VerifiedModuleReport verify_module_with_diagnostics(Module module) {
    auto verification_report = verify_with_diagnostics(module);

    VerifiedModuleReport report;
    report.diagnostics = std::move(verification_report.diagnostics);
    if (!verification_report.result.has_value()) {
        return report;
    }

    auto verification = std::move(*verification_report.result);
    for (std::size_t i = 0; i < module.functions.size(); ++i) {
        module.functions[i].stack_maps = verification.functions[i].stack_maps;
    }
    report.module = VerifiedModule(std::move(module), std::move(verification));
    return report;
}

std::optional<VerificationResult> verify_with_stack_maps(const Function& function) {
    auto report = verify_with_diagnostics(function);
    if (!report.result.has_value()) {
        return std::nullopt;
    }
    return std::move(report.result);
}

std::optional<ModuleVerificationResult> verify_with_stack_maps(const Module& module) {
    auto report = verify_with_diagnostics(module);
    if (!report.result.has_value()) {
        return std::nullopt;
    }
    return std::move(report.result);
}

std::optional<VerifiedModule> verify_module(Module module) {
    auto report = verify_module_with_diagnostics(std::move(module));
    if (!report.module.has_value()) {
        return std::nullopt;
    }
    return std::move(report.module);
}

bool verify(const Function& function) {
    return verify_with_stack_maps(function).has_value();
}

bool verify(const Module& module) {
    return verify_with_stack_maps(module).has_value();
}

} // namespace lang
