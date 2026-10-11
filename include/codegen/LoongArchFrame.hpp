#pragma once
#include "MachineIR.hpp"
namespace machine {
void lower_frame(MachineFunction &function);
std::string print_assembly(const MachineModule &module);
} // namespace machine
