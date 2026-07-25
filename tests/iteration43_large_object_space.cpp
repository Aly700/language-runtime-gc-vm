#include "lang/gc/heap.hpp"

#include <cstddef>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

class OrderedRoots final : public lang::gc::RootProvider {
public:
    void add(lang::ObjectId id) {
        values_.push_back(lang::Value::object(id));
    }

    void trace_roots(lang::gc::RootVisitor& visitor) override {
        for (auto& value : values_) {
            visitor.visit(value);
        }
    }

private:
    std::vector<lang::Value> values_;
};

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_passive_compaction_copy_size_classes() {
    lang::gc::Heap heap;
    OrderedRoots roots;
    heap.set_root_provider(&roots);

    roots.add(heap.allocate_pair(lang::Value::int64(1),
                                 lang::Value::int64(2)));
    for (const auto width : {
             std::size_t{8}, std::size_t{9}, std::size_t{64},
             std::size_t{65}, std::size_t{512}, std::size_t{513}}) {
        roots.add(heap.allocate_scalar_array(width, 7));
    }

    heap.collect();
    const auto metrics = heap.metrics();
    require(metrics.compaction_objects_copied == 7,
            "major collection did not copy every rooted survivor");
    require(metrics.compaction_slots_copied_1_8 == 9,
            "1..8 copied-slot class did not include widths 1 and 8");
    require(metrics.compaction_slots_copied_9_64 == 73,
            "9..64 copied-slot class did not include boundary widths");
    require(metrics.compaction_slots_copied_65_512 == 577,
            "65..512 copied-slot class did not include boundary widths");
    require(metrics.compaction_slots_copied_gt_512 == 513,
            ">512 copied-slot class did not include width 513");
    require(metrics.max_object_storage_slots_allocated == 513,
            "maximum allocated logical width was not observed");
}

} // namespace

int main() {
    try {
        test_passive_compaction_copy_size_classes();
        std::cout << "iteration43_large_object_space: ok\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "iteration43_large_object_space: " << error.what()
                  << "\n";
        return 1;
    }
}
