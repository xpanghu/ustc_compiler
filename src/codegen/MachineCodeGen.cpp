#include "MachineCodeGen.hpp"
#include "LoongArchISel.hpp"
#include "MachineLowering.hpp"
#include "StackAllocator.hpp"
#include "LoongArchFrame.hpp"

namespace machine {
CodeGenResult generate(Module &module, const Options &options) {
    auto selected = select(module, options);
    verify(selected, VerifyStage::Selected);
    CodeGenResult result;
    result.selected = dump(selected);
    lower(selected, options);
    verify(selected, VerifyStage::PreRA);
    result.prera = dump(selected);
    for (auto &function : selected.functions) {
        allocate_stack(function);
        lower_frame(function);
    }
    verify(selected, VerifyStage::Final);
    result.final = dump(selected);
    result.assembly = print_assembly(selected);
    return result;
}
} // namespace machine
