#pragma once
#include "MachineIR.hpp"
class Module;
namespace machine {
struct CodeGenResult {
    std::string selected, prera, final, assembly;
};
CodeGenResult generate(Module &module, const Options &options = {});
} // namespace machine
