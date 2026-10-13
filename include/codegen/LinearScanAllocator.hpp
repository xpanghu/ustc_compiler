#pragma once
#include "MachineLiveness.hpp"
#include <variant>
namespace machine {
struct RegisterSet {
    std::vector<PReg> gprs, fprs;
};
struct SpillLocation {
    unsigned slot;
};
using Location = std::variant<PReg, SpillLocation>;
struct SpillSlot {
    VReg value;
    ValueKind kind;
    unsigned size, alignment;
};
struct AllocationStats {
    // Retained counts all values with an interval; spilled is a subset.
    unsigned retained = 0, spilled = 0;
    uint64_t reloads = 0, stores = 0;
};
struct AllocationResult {
    std::vector<std::optional<Location>> locations;
    std::vector<SpillSlot> spills;
    std::vector<unsigned> spill_frames;
    std::vector<PReg> used_callee_saved;
    AllocationStats stats;
};
RegisterSet default_registers();
AllocationResult linear_scan(const MachineFunction &, const LiveAnalysis &, const RegisterSet &);
void rewrite_allocation(MachineFunction &, AllocationResult &);
AllocationResult allocate_linear(MachineFunction &);
std::string dump_allocation(const MachineFunction &, const LiveAnalysis &,
                            const AllocationResult &);
} // namespace machine
