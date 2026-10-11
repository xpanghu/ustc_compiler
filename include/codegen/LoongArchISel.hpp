#pragma once
#include "MachineIR.hpp"
class Module;
namespace machine {
MachineModule select(Module &module, const Options &options);
}
