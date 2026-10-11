#pragma once
#include "MachineIR.hpp"
namespace machine {
void lower(MachineModule &module, const Options &options);
std::vector<MachineInstr>
parallel_copies(MachineFunction &function,
                std::vector<std::pair<MachineOperand, MachineOperand>> copies);
} // namespace machine
