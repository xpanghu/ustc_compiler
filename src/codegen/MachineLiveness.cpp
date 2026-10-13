#include "MachineLiveness.hpp"
#include <algorithm>
#include <utility>
namespace machine {
bool overlaps(Range a, Range b) { return a.begin < b.end && b.begin < a.end; }
static std::pair<LiveSet, LiveSet> instruction_access(const MachineInstr &instruction) {
    LiveSet use, def;
    for (const auto &operand : instruction.operands) {
        if (operand.is_use()) {
            if (operand.kind == OperandKind::VReg)
                use.virtuals.insert(operand.id);
            else
                use.physicals.insert(operand.reg);
        }
        if (operand.is_def()) {
            if (operand.kind == OperandKind::VReg)
                def.virtuals.insert(operand.id);
            else
                def.physicals.insert(operand.reg);
        }
    }
    const auto &desc = descriptor(instruction.opcode);
    use.physicals.insert(desc.implicit_uses.begin(), desc.implicit_uses.end());
    def.physicals.insert(desc.implicit_defs.begin(), desc.implicit_defs.end());
    def.physicals.insert(desc.clobbers.begin(), desc.clobbers.end());
    return {std::move(use), std::move(def)};
}
template <class T> static void add_use(std::set<T> &use, const std::set<T> &def, T value) {
    if (!def.count(value))
        use.insert(value);
}
template <class T>
static std::set<T> transfer(const std::set<T> &out, const std::set<T> &use,
                            const std::set<T> &def) {
    std::set<T> in = use;
    for (const auto &value : out)
        if (!def.count(value))
            in.insert(value);
    return in;
}
static void visit(BlockId id, const std::map<BlockId, const MachineBasicBlock *> &blocks,
                  std::set<BlockId> &visited, std::vector<BlockId> &postorder) {
    if (!visited.insert(id).second)
        return;
    auto successors = blocks.at(id)->successors;
    std::sort(successors.begin(), successors.end());
    for (auto successor : successors)
        visit(successor, blocks, visited, postorder);
    postorder.push_back(id);
}
static void touch(std::vector<std::optional<LiveInterval>> &intervals, VReg value, Position p) {
    auto &entry = intervals.at(value);
    if (!entry)
        entry = LiveInterval{value, {p, p + 1}};
    else {
        entry->range.begin = std::min(entry->range.begin, p);
        entry->range.end = std::max(entry->range.end, p + 1);
    }
}
static void touch(std::map<PReg, std::vector<Range>> &fixed, PReg reg, Position p) {
    fixed[reg].push_back({p, p + 1});
}
LiveAnalysis analyze_liveness(const MachineFunction &function) {
    LiveAnalysis analysis;
    std::map<BlockId, const MachineBasicBlock *> blocks;
    for (const auto &block : function.blocks) {
        blocks.emplace(block.id, &block);
        auto &live = analysis.blocks[block.id];
        for (const auto &instruction : block.instructions) {
            auto [use, def] = instruction_access(instruction);
            for (auto value : use.virtuals)
                add_use(live.use.virtuals, live.def.virtuals, value);
            for (auto reg : use.physicals)
                add_use(live.use.physicals, live.def.physicals, reg);
            live.def.virtuals.insert(def.virtuals.begin(), def.virtuals.end());
            live.def.physicals.insert(def.physicals.begin(), def.physicals.end());
        }
    }
    bool changed;
    do {
        changed = false;
        for (auto block = function.blocks.rbegin(); block != function.blocks.rend(); ++block) {
            auto &live = analysis.blocks.at(block->id);
            LiveSet out;
            for (auto successor : block->successors) {
                const auto &in = analysis.blocks.at(successor).in;
                out.virtuals.insert(in.virtuals.begin(), in.virtuals.end());
                out.physicals.insert(in.physicals.begin(), in.physicals.end());
            }
            LiveSet in{transfer(out.virtuals, live.use.virtuals, live.def.virtuals),
                       transfer(out.physicals, live.use.physicals, live.def.physicals)};
            changed |= in.virtuals != live.in.virtuals || in.physicals != live.in.physicals ||
                       out.virtuals != live.out.virtuals || out.physicals != live.out.physicals;
            live.in = std::move(in);
            live.out = std::move(out);
        }
    } while (changed);
    std::set<BlockId> visited;
    auto append_rpo = [&](BlockId root) {
        std::vector<BlockId> postorder;
        visit(root, blocks, visited, postorder);
        analysis.order.insert(analysis.order.end(), postorder.rbegin(), postorder.rend());
    };
    if (!function.blocks.empty())
        append_rpo(function.blocks.front().id);
    for (const auto &[id, block] : blocks)
        if (!visited.count(id))
            append_rpo(id);
    Position next = 0;
    analysis.intervals.resize(function.vreg_types.size());
    for (auto id : analysis.order) {
        const auto &block = *blocks.at(id);
        auto &live = analysis.blocks.at(id);
        live.entry = next++;
        for (size_t i = 0; i < block.instructions.size(); ++i)
            live.instructions.push_back(next++);
        live.exit = next++;
        for (auto value : live.in.virtuals)
            touch(analysis.intervals, value, live.entry);
        for (auto value : live.out.virtuals)
            touch(analysis.intervals, value, live.exit);
        for (auto reg : live.in.physicals)
            touch(analysis.fixed, reg, live.entry);
        for (auto reg : live.out.physicals)
            touch(analysis.fixed, reg, live.exit);
        auto physical_after = live.out.physicals;
        auto after = live.out.virtuals;
        for (size_t i = block.instructions.size(); i > 0; --i) {
            auto [use, def] = instruction_access(block.instructions[i - 1]);
            auto before = transfer(after, use.virtuals, def.virtuals);
            const auto position = live.instructions[i - 1];
            for (const auto *occupied : {&before, &after, &use.virtuals, &def.virtuals})
                for (auto value : *occupied)
                    touch(analysis.intervals, value, position);
            auto physical_before = transfer(physical_after, use.physicals, def.physicals);
            for (const auto *occupied :
                 {&physical_before, &physical_after, &use.physicals, &def.physicals})
                for (auto reg : *occupied)
                    touch(analysis.fixed, reg, position);
            physical_after = std::move(physical_before);
            after = std::move(before);
        }
    }
    for (auto &[reg, ranges] : analysis.fixed) {
        std::sort(ranges.begin(), ranges.end(), [](Range a, Range b) { return a.begin < b.begin; });
        std::vector<Range> merged;
        for (auto next : ranges) {
            if (!merged.empty() && next.begin <= merged.back().end)
                merged.back().end = std::max(merged.back().end, next.end);
            else
                merged.push_back(next);
        }
        ranges = std::move(merged);
    }
    return analysis;
}
} // namespace machine
