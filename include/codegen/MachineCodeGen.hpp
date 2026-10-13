#pragma once
#include "MachineIR.hpp"
class Module;
namespace machine {
struct CodeGenResult {
    std::string selected, prera, final, assembly;
    std::string allocated, regalloc;
};
CodeGenResult generate(Module &module, const Options &options = {});
} // namespace machine
