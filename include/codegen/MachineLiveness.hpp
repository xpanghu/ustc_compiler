#pragma once
#include "MachineIR.hpp"
#include <map>
#include <optional>
#include <set>
namespace machine {
using Position = uint64_t;
struct Range {
    Position begin = 0, end = 0;
};
struct LiveInterval {
    VReg value;
    Range range;
};
struct LiveSet {
    std::set<VReg> virtuals;
    std::set<PReg> physicals;
};
struct BlockLiveness {
    LiveSet use, def, in, out;
    Position entry = 0, exit = 0;
    std::vector<Position> instructions;
};
struct LiveAnalysis {
    std::vector<BlockId> order;
    std::map<BlockId, BlockLiveness> blocks;
    std::vector<std::optional<LiveInterval>> intervals;
    std::map<PReg, std::vector<Range>> fixed;
};
LiveAnalysis analyze_liveness(const MachineFunction &function);
bool overlaps(Range a, Range b);
} // namespace machine
