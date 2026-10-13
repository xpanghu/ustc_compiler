#include "LinearScanAllocator.hpp"
#include <algorithm>
#include <tuple>
#include <stdexcept>
#include <set>
#include <sstream>
namespace machine {
RegisterSet default_registers() {
    return {allocatable_registers(RegClass::GPR), allocatable_registers(RegClass::FPR)};
}
AllocationResult linear_scan(const MachineFunction &function, const LiveAnalysis &analysis,
                             const RegisterSet &registers) {
    AllocationResult result;
    result.locations.resize(function.vreg_types.size());
    for (auto cls : {RegClass::GPR, RegClass::FPR}) {
        const auto &regs = cls == RegClass::GPR ? registers.gprs : registers.fprs;
        std::vector<VReg> order, active;
        for (const auto &interval : analysis.intervals)
            if (interval && reg_class(function.vreg_types.at(interval->value)) == cls)
                order.push_back(interval->value);
        std::sort(order.begin(), order.end(), [&](VReg x, VReg y) {
            const auto &a = analysis.intervals.at(x)->range;
            const auto &b = analysis.intervals.at(y)->range;
            return std::tie(a.begin, a.end, x) < std::tie(b.begin, b.end, y);
        });
        auto active_less = [&](VReg x, VReg y) {
            const auto &a = analysis.intervals.at(x)->range;
            const auto &b = analysis.intervals.at(y)->range;
            return std::tie(a.end, a.begin, x) < std::tie(b.end, b.begin, y);
        };
        for (auto value : order) {
            const auto &current = *analysis.intervals.at(value);
            active.erase(std::remove_if(active.begin(), active.end(),
                                        [&](VReg x) {
                                            return analysis.intervals.at(x)->range.end <=
                                                   current.range.begin;
                                        }),
                         active.end());
            std::vector<PReg> legal;
            for (auto reg : regs) {
                const auto fixed = analysis.fixed.find(reg);
                if (fixed == analysis.fixed.end() ||
                    std::none_of(fixed->second.begin(), fixed->second.end(),
                                 [&](Range range) { return overlaps(current.range, range); }))
                    legal.push_back(reg);
            }
            const auto free = std::find_if(legal.begin(), legal.end(), [&](PReg reg) {
                return std::none_of(active.begin(), active.end(), [&](VReg x) {
                    return std::get<PReg>(*result.locations.at(x)) == reg;
                });
            });
            if (free != legal.end()) {
                result.locations.at(value) = *free;
            } else {
                std::optional<VReg> victim;
                for (auto x : active) {
                    const auto reg = std::get<PReg>(*result.locations.at(x));
                    if (std::find(legal.begin(), legal.end(), reg) != legal.end() &&
                        (!victim || active_less(*victim, x)))
                        victim = x;
                }
                if (!victim || analysis.intervals.at(*victim)->range.end <= current.range.end)
                    continue;
                result.locations.at(value) = result.locations.at(*victim);
                result.locations.at(*victim).reset();
                active.erase(std::find(active.begin(), active.end(), *victim));
            }
            active.push_back(value);
            std::sort(active.begin(), active.end(), active_less);
        }
    }
    for (VReg value = 0; value < function.vreg_types.size(); ++value) {
        if (!analysis.intervals.at(value))
            continue;
        ++result.stats.retained;
        auto &location = result.locations.at(value);
        if (!location) {
            location = SpillLocation{static_cast<unsigned>(result.spills.size())};
            const auto kind = function.vreg_types.at(value);
            const unsigned size = kind == ValueKind::Ptr64 ? 8 : 4;
            result.spills.push_back({value, kind, size, size});
            ++result.stats.spilled;
        } else {
            const auto reg = std::get<PReg>(*location);
            if (is_callee_saved(reg))
                result.used_callee_saved.push_back(reg);
        }
    }
    auto &saved = result.used_callee_saved;
    std::sort(saved.begin(), saved.end());
    saved.erase(std::unique(saved.begin(), saved.end()), saved.end());
    return result;
}
void verify_allocation(const MachineFunction &function, const LiveAnalysis &analysis,
                       const RegisterSet &registers, const AllocationResult &result) {
    auto fail = [&](const std::string &message) {
        throw std::runtime_error("allocation in " + function.name + ": " + message);
    };
    auto value_name = [](VReg value) { return "v" + std::to_string(value); };
    if (result.locations.size() != function.vreg_types.size())
        fail("location count does not match VReg count");
    std::map<PReg, std::vector<VReg>> assigned;
    std::vector<unsigned> spill_references(result.spills.size());
    std::set<PReg> expected_saved;
    for (VReg value = 0; value < function.vreg_types.size(); ++value) {
        const auto &interval = analysis.intervals.at(value);
        const auto &location = result.locations.at(value);
        const auto name = value_name(value);
        if (!interval) {
            if (location)
                fail(name + " has a location without an interval");
            continue;
        }
        if (!location)
            fail(name + " has no location");
        const auto kind = function.vreg_types.at(value);
        if (std::holds_alternative<PReg>(*location)) {
            const auto reg = std::get<PReg>(*location);
            const auto context = name + " in " + register_name(reg);
            const auto cls = reg_class(kind);
            if (reg.cls != cls)
                fail(context + " has wrong register class");
            const auto &target = allocatable_registers(cls);
            const auto &bank = cls == RegClass::GPR ? registers.gprs : registers.fprs;
            if (std::find(target.begin(), target.end(), reg) == target.end() ||
                std::find(bank.begin(), bank.end(), reg) == bank.end())
                fail(context + " is not in the allocatable register set");
            const auto fixed = analysis.fixed.find(reg);
            if (fixed != analysis.fixed.end())
                for (auto range : fixed->second)
                    if (overlaps(interval->range, range))
                        fail(context + " overlaps fixed occupancy");
            assigned[reg].push_back(value);
            if (is_callee_saved(reg))
                expected_saved.insert(reg);
        } else {
            const auto slot = std::get<SpillLocation>(*location).slot;
            if (slot >= result.spills.size())
                fail(name + " references missing spill slot " + std::to_string(slot));
            const auto &spill = result.spills.at(slot);
            if (spill.value != value)
                fail(name + " references spill owned by " + value_name(spill.value));
            if (++spill_references.at(slot) != 1)
                fail(name + " shares spill slot " + std::to_string(slot));
            const unsigned size = kind == ValueKind::Ptr64 ? 8 : 4;
            if (spill.kind != kind || spill.size != size || spill.alignment != size)
                fail(name + " has invalid spill kind, size or alignment");
        }
    }
    for (const auto &[reg, values] : assigned)
        for (size_t i = 0; i < values.size(); ++i)
            for (size_t j = i + 1; j < values.size(); ++j)
                if (overlaps(analysis.intervals.at(values[i])->range,
                             analysis.intervals.at(values[j])->range))
                    fail(value_name(values[i]) + " and " + value_name(values[j]) + " overlap in " +
                         register_name(reg));
    for (size_t slot = 0; slot < result.spills.size(); ++slot)
        if (spill_references.at(slot) != 1)
            fail(value_name(result.spills.at(slot).value) + " has unreferenced spill slot " +
                 std::to_string(slot));
    std::set<PReg> actual_saved;
    for (auto reg : result.used_callee_saved) {
        if (!actual_saved.insert(reg).second)
            fail(register_name(reg) + " is duplicated in used_callee_saved");
        if (!expected_saved.count(reg))
            fail(register_name(reg) + " is unexpected in used_callee_saved");
    }
    for (auto reg : expected_saved)
        if (!actual_saved.count(reg))
            fail(value_name(assigned.at(reg).front()) + " in " + register_name(reg) +
                 " is missing from used_callee_saved");
}
namespace {
using O = MachineOperand;
Opcode reload_opcode(ValueKind kind) {
    return kind == ValueKind::F32     ? Opcode::FLD_S
           : kind == ValueKind::Ptr64 ? Opcode::LD_D
                                      : Opcode::LD_W;
}
Opcode spill_opcode(ValueKind kind) {
    return kind == ValueKind::F32     ? Opcode::FST_S
           : kind == ValueKind::Ptr64 ? Opcode::ST_D
                                      : Opcode::ST_W;
}
} // namespace
void rewrite_allocation(MachineFunction &function, AllocationResult &result) {
    // Fixed occupancy must describe the original PreRA instruction positions.
    const auto analysis = analyze_liveness(function);
    for (const auto &slot : result.spills) {
        result.spill_frames.push_back(function.frame_objects.size());
        function.frame_objects.push_back(
            {slot.kind, slot.size, slot.alignment, FramePurpose::Spill});
    }
    function.used_callee_saved = result.used_callee_saved;
    for (auto &block : function.blocks) {
        std::vector<MachineInstr> output;
        for (size_t n = 0; n < block.instructions.size(); ++n) {
            auto instruction = block.instructions[n];
            auto fail = [&](const std::string &message) {
                throw std::runtime_error("allocation rewrite in " + function.name + " bb" +
                                         std::to_string(block.id) + " instruction " +
                                         std::to_string(n) + ": " + message);
            };
            auto scratch = [&](ValueKind kind, unsigned index, VReg value) {
                if (index >= 3)
                    fail("v" + std::to_string(value) + " exceeds reserved scratch registers");
                const auto reg = kind == ValueKind::F32 ? fpr(8 + index) : gpr(12 + index);
                const auto fixed = analysis.fixed.find(reg);
                const auto position = analysis.blocks.at(block.id).instructions.at(n);
                if (fixed != analysis.fixed.end())
                    for (auto range : fixed->second)
                        if (overlaps({position, position + 1}, range))
                            fail("v" + std::to_string(value) + " scratch conflict in " +
                                 register_name(reg));
                return reg;
            };
            auto location = [&](const O &operand) -> Location {
                if (operand.kind == OperandKind::PReg)
                    return operand.reg;
                const auto &loc = result.locations.at(operand.id);
                if (!loc)
                    fail("v" + std::to_string(operand.id) + " has no location");
                return *loc;
            };
            auto frame = [&](VReg value) {
                return result.spill_frames.at(
                    std::get<SpillLocation>(*result.locations.at(value)).slot);
            };
            auto reload = [&](VReg value, PReg reg) {
                output.push_back(
                    {reload_opcode(function.vreg_types.at(value)),
                     {O::preg(reg, RegRole::Def), O::frame(frame(value)), O::immediate(0)},
                     instruction.source});
                ++result.stats.reloads;
            };
            auto spill = [&](VReg value, PReg reg) {
                output.push_back({spill_opcode(function.vreg_types.at(value)),
                                  {O::preg(reg), O::frame(frame(value)), O::immediate(0)},
                                  instruction.source});
                ++result.stats.stores;
            };
            if (instruction.opcode == Opcode::COPY) {
                const auto dst = instruction.operands.at(0), src = instruction.operands.at(1);
                const auto destination = location(dst), source = location(src);
                const auto *dr = std::get_if<PReg>(&destination);
                const auto *sr = std::get_if<PReg>(&source);
                if (dr && sr) {
                    if (*dr != *sr) {
                        instruction.operands = {O::preg(*dr, RegRole::Def), O::preg(*sr)};
                        output.push_back(std::move(instruction));
                    }
                } else if (dr) {
                    reload(src.id, *dr);
                } else if (sr) {
                    spill(dst.id, *sr);
                } else {
                    const auto reg = scratch(function.vreg_types.at(src.id), 0, src.id);
                    reload(src.id, reg);
                    spill(dst.id, reg);
                }
                continue;
            }
            std::map<VReg, PReg> bindings;
            unsigned gpr_uses = 0, fpr_uses = 0;
            // Bind every input before considering destinations; repeated uses reload once.
            for (const auto &operand : instruction.operands) {
                if (operand.kind != OperandKind::VReg || !operand.is_use() ||
                    bindings.count(operand.id))
                    continue;
                const auto loc = location(operand);
                if (const auto *resident = std::get_if<PReg>(&loc)) {
                    bindings.emplace(operand.id, *resident);
                } else {
                    const auto kind = function.vreg_types.at(operand.id);
                    const auto reg =
                        scratch(kind, kind == ValueKind::F32 ? fpr_uses++ : gpr_uses++, operand.id);
                    bindings.emplace(operand.id, reg);
                    reload(operand.id, reg);
                }
            }
            std::set<PReg> definition_scratch;
            // A definition of an input keeps its binding, including separate use/def operands.
            for (const auto &operand : instruction.operands)
                if (operand.kind == OperandKind::VReg && operand.is_def() &&
                    bindings.count(operand.id) &&
                    std::holds_alternative<SpillLocation>(location(operand)))
                    definition_scratch.insert(bindings.at(operand.id));
            std::map<VReg, PReg> definitions;
            for (auto &operand : instruction.operands) {
                if (operand.kind != OperandKind::VReg)
                    continue;
                const auto value = operand.id;
                auto binding = bindings.find(value);
                if (binding == bindings.end()) {
                    const auto loc = location(operand);
                    PReg reg;
                    if (const auto *resident = std::get_if<PReg>(&loc)) {
                        reg = *resident;
                    } else {
                        const auto kind = function.vreg_types.at(value);
                        unsigned index = 0;
                        while (index < 3 &&
                               definition_scratch.count(kind == ValueKind::F32 ? fpr(8 + index)
                                                                               : gpr(12 + index)))
                            ++index;
                        reg = scratch(kind, index, value);
                        definition_scratch.insert(reg);
                    }
                    binding = bindings.emplace(value, reg).first;
                }
                if (operand.is_def() &&
                    std::holds_alternative<SpillLocation>(*result.locations.at(value)))
                    definitions.emplace(value, binding->second);
                operand = O::preg(binding->second, operand.role);
            }
            output.push_back(instruction);
            for (const auto &[value, reg] : definitions)
                spill(value, reg);
        }
        block.instructions = std::move(output);
    }
}
AllocationResult allocate_linear(MachineFunction &function) {
    const auto analysis = analyze_liveness(function);
    const auto registers = default_registers();
    auto result = linear_scan(function, analysis, registers);
    verify_allocation(function, analysis, registers, result);
    rewrite_allocation(function, result);
    return result;
}
std::string dump_allocation(const MachineFunction &function, const LiveAnalysis &analysis,
                            const AllocationResult &result) {
    std::ostringstream output;
    output << "function @" << function.name << "\n";
    auto live_set = [&](const LiveSet &set) {
        output << "{";
        const char *separator = "";
        for (auto value : set.virtuals) {
            output << separator << "v" << value;
            separator = ", ";
        }
        for (auto reg : set.physicals) {
            output << separator << register_name(reg);
            separator = ", ";
        }
        output << "}";
    };
    for (const auto &[id, block] : analysis.blocks) {
        output << "  bb" << id << " IN=";
        live_set(block.in);
        output << " OUT=";
        live_set(block.out);
        output << "\n";
    }
    output << "  intervals/locations\n";
    for (VReg value = 0; value < analysis.intervals.size(); ++value) {
        if (!analysis.intervals[value])
            continue;
        const auto range = analysis.intervals[value]->range;
        output << "    v" << value << " [" << range.begin << "," << range.end << ") -> ";
        const auto &location = *result.locations.at(value);
        if (const auto *reg = std::get_if<PReg>(&location))
            output << register_name(*reg);
        else
            output << "slot" << std::get<SpillLocation>(location).slot;
        output << "\n";
    }
    output << "  fixed ranges\n";
    for (const auto &[reg, ranges] : analysis.fixed) {
        output << "    " << register_name(reg);
        for (auto range : ranges)
            output << " [" << range.begin << "," << range.end << ")";
        output << "\n";
    }
    output << "  spill slots\n";
    const char *kinds[] = {"I1", "I32", "Ptr64", "F32"};
    for (unsigned slot = 0; slot < result.spills.size(); ++slot) {
        const auto &spill = result.spills[slot];
        output << "    slot" << slot << " v" << spill.value << " " << kinds[unsigned(spill.kind)]
               << " size=" << spill.size << " alignment=" << spill.alignment;
        if (slot < result.spill_frames.size())
            output << " frame=fi" << result.spill_frames[slot];
        output << "\n";
    }
    auto saved = result.used_callee_saved;
    std::sort(saved.begin(), saved.end());
    output << "  callee_saved={";
    const char *separator = "";
    for (auto reg : saved) {
        output << separator << register_name(reg);
        separator = ", ";
    }
    output << "}\n  stats retained=" << result.stats.retained << " spilled=" << result.stats.spilled
           << " reloads=" << result.stats.reloads << " stores=" << result.stats.stores
           << " callee_saved=" << saved.size() << " frame_size=" << function.frame_size << "\n";
    return output.str();
}
} // namespace machine
