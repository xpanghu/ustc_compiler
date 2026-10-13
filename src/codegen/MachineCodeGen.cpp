#include "MachineCodeGen.hpp"
#include "LoongArchISel.hpp"
#include "MachineLowering.hpp"
#include "StackAllocator.hpp"
#include "LoongArchFrame.hpp"
#include "LinearScanAllocator.hpp"

namespace machine {
CodeGenResult generate(Module &module, const Options &options) {
    auto selected = select(module, options);
    verify(selected, VerifyStage::Selected);
    CodeGenResult result;
    result.selected = dump(selected);
    lower(selected, options);
    verify(selected, VerifyStage::PreRA);
    result.prera = dump(selected);
    std::vector<LiveAnalysis> analyses;
    std::vector<AllocationResult> allocations;
    for (auto &function : selected.functions) {
        if (options.allocation == AllocationMode::Linear) {
            analyses.push_back(analyze_liveness(function));
            const auto registers = default_registers();
            allocations.push_back(linear_scan(function, analyses.back(), registers));
            verify_allocation(function, analyses.back(), registers, allocations.back());
            rewrite_allocation(function, allocations.back());
        } else {
            allocate_stack(function);
        }
    }
    verify(selected, VerifyStage::Allocated);
    result.allocated = dump(selected);
    for (auto &function : selected.functions)
        lower_frame(function);
    verify(selected, VerifyStage::Final);
    result.final = dump(selected);
    result.assembly = print_assembly(selected);
    if (options.allocation == AllocationMode::Linear)
        for (size_t i = 0; i < selected.functions.size(); ++i)
            result.regalloc += dump_allocation(selected.functions[i], analyses[i], allocations[i]);
    return result;
}
} // namespace machine
