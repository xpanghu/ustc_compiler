#include "MachineLiveness.hpp"
#include "LinearScanAllocator.hpp"
#include "MachineLowering.hpp"
#include "LoongArchFrame.hpp"
#include "StackAllocator.hpp"
#include "MachineCodeGen.hpp"
#include "Module.hpp"
#include "IRBuilder.hpp"
#include "Function.hpp"
#include "BasicBlock.hpp"
#include "Constant.hpp"
#include <algorithm>
#include <iostream>
#include <functional>
#include <fstream>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
using namespace machine;
static void require(bool ok, const char *message) {
    if (!ok)
        throw std::runtime_error(message);
}
static bool same_set(const LiveSet &a, const LiveSet &b) {
    return a.virtuals == b.virtuals && a.physicals == b.physicals;
}
static LiveAnalysis analyze_checked(const MachineFunction &f) {
    const auto before = dump(MachineModule{{f}, {}});
    auto a = analyze_liveness(f), b = analyze_liveness(f);
    require(before == dump(MachineModule{{f}, {}}), "analysis must preserve function dump");
    require(a.order == b.order && a.blocks.size() == b.blocks.size() &&
                a.intervals.size() == b.intervals.size() && a.fixed.size() == b.fixed.size(),
            "analysis shape must be deterministic");
    for (const auto &[id, x] : a.blocks) {
        const auto &y = b.blocks.at(id);
        require(same_set(x.use, y.use) && same_set(x.def, y.def) && same_set(x.in, y.in) &&
                    same_set(x.out, y.out) && x.entry == y.entry && x.exit == y.exit &&
                    x.instructions == y.instructions,
                "block analysis must be deterministic");
    }
    for (size_t i = 0; i < a.intervals.size(); ++i) {
        const auto &x = a.intervals[i], &y = b.intervals[i];
        require(x.has_value() == y.has_value(), "interval presence must be deterministic");
        if (x)
            require(x->value == y->value && x->range.begin == y->range.begin &&
                        x->range.end == y->range.end,
                    "interval range must be deterministic");
    }
    for (const auto &[reg, ranges] : a.fixed) {
        const auto &other = b.fixed.at(reg);
        require(ranges.size() == other.size(), "fixed range count must be deterministic");
        for (size_t i = 0; i < ranges.size(); ++i)
            require(ranges[i].begin == other[i].begin && ranges[i].end == other[i].end,
                    "fixed ranges must be deterministic");
    }
    return a;
}
static void require_interval(const LiveAnalysis &a, VReg value, Position begin, Position end) {
    const auto &interval = a.intervals.at(value);
    require(interval && interval->value == value && interval->range.begin == begin &&
                interval->range.end == end,
            "unexpected virtual interval");
}
static void liveness_chain(bool intervals = false) {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "liveness_chain";
    auto x = f.new_vreg(ValueKind::I32);
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(7)}},
                  {Opcode::B, {O::block(1)}}},
                 {},
                 {}},
                {1,
                 "exit",
                 {{Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(x)}},
                  {Opcode::RET, {O::preg(gpr(4))}}},
                 {},
                 {}}};
    rebuild_cfg(f);
    auto a = analyze_checked(f);
    require(a.blocks.at(0).in.virtuals.empty(), "entry should not read x");
    require(a.blocks.at(0).out.virtuals == std::set<VReg>{x}, "x must cross edge");
    require(a.blocks.at(1).in.virtuals == std::set<VReg>{x}, "exit must receive x");
    require(a.blocks.at(1).out.virtuals.empty(), "no virtual live after return");
    require(a.blocks.at(0).use.virtuals.empty(), "definition must hide later use");
    require(a.blocks.at(0).def.virtuals == std::set<VReg>{x}, "entry must define x");
    require(a.blocks.at(1).use.virtuals == std::set<VReg>{x}, "exit must read x");
    if (intervals) {
        require(a.blocks.at(0).entry == 0 && a.blocks.at(0).exit == 3, "entry numbering");
        require(a.blocks.at(1).entry == 4 && a.blocks.at(1).exit == 7, "exit numbering");
        require(a.blocks.at(0).instructions == std::vector<Position>{1, 2},
                "entry instruction slots");
        require(a.blocks.at(1).instructions == std::vector<Position>{5, 6},
                "exit instruction slots");
        require(a.intervals.at(x)->range.begin == 1, "definition slot");
        require(a.intervals.at(x)->range.end == 6, "last use exclusive end");
    }
}
static void liveness_diamond() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "liveness_diamond";
    auto x = f.new_vreg(ValueKind::I32);
    f.blocks = {{10,
                 "entry",
                 {{Opcode::BNEZ, {O::preg(gpr(4)), O::block(20)}}, {Opcode::B, {O::block(30)}}},
                 {},
                 {}},
                {20,
                 "left",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(1)}},
                  {Opcode::B, {O::block(40)}}},
                 {},
                 {}},
                {30,
                 "right",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(2)}},
                  {Opcode::B, {O::block(40)}}},
                 {},
                 {}},
                {40,
                 "join",
                 {{Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(x)}},
                  {Opcode::RET, {O::preg(gpr(4))}}},
                 {},
                 {}}};
    rebuild_cfg(f);
    auto a = analyze_checked(f);
    require(a.blocks.at(40).in.virtuals == std::set<VReg>{x}, "join must receive x");
    for (BlockId id : {20u, 30u}) {
        require(a.blocks.at(id).out.virtuals == std::set<VReg>{x}, "both arms must supply x");
        require(a.blocks.at(id).in.virtuals.empty(), "arm definition must kill incoming x");
    }
    require(a.blocks.at(10).out.virtuals.empty(), "arm definitions must stop x at entry");
}
static void liveness_loop() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "liveness_loop";
    auto x = f.new_vreg(ValueKind::Ptr64);
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(7)}},
                  {Opcode::B, {O::block(1)}}},
                 {},
                 {}},
                {1,
                 "loop",
                 {{Opcode::LU32I_D, {O::vreg(x, RegRole::UseDef), O::immediate(1)}},
                  {Opcode::BNEZ, {O::vreg(x), O::block(1)}},
                  {Opcode::B, {O::block(2)}}},
                 {},
                 {}},
                {2,
                 "exit",
                 {{Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(x)}},
                  {Opcode::RET, {O::preg(gpr(4))}}},
                 {},
                 {}}};
    rebuild_cfg(f);
    auto a = analyze_checked(f);
    const auto &loop = a.blocks.at(1);
    require(loop.use.virtuals == std::set<VReg>{x}, "UseDef must read before writing");
    require(loop.def.virtuals == std::set<VReg>{x}, "UseDef must define x");
    require(loop.in.virtuals == std::set<VReg>{x}, "loop must receive x");
    require(loop.out.virtuals == std::set<VReg>{x}, "loop must keep x across back edge");
    require(a.blocks.at(0).in.virtuals.empty(), "entry definition must stop loop input");
}
static void liveness_physical() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "liveness_physical";
    f.blocks = {
        {0,
         "call",
         {{Opcode::COPY, {O::preg(gpr(22), RegRole::Def), O::preg(gpr(22))}},
          {Opcode::CALL,
           {O::sym("callee"), O::preg(gpr(4)), O::preg(fpr(0)), O::preg(gpr(4), RegRole::Def)}},
          {Opcode::B, {O::block(1)}}},
         {},
         {}},
        {1,
         "exit",
         {{Opcode::COPY, {O::preg(fpr(24), RegRole::Def), O::preg(fpr(24))}},
          {Opcode::MOVCF2GR, {O::preg(gpr(4), RegRole::Def), O::preg(fcc(0))}},
          {Opcode::RET, {O::preg(gpr(4))}}},
         {},
         {}}};
    rebuild_cfg(f);
    auto a = analyze_checked(f);
    const auto &call = a.blocks.at(0), &exit = a.blocks.at(1);
    auto defs = std::set<PReg>(caller_saved().begin(), caller_saved().end());
    defs.insert(gpr(22));
    require(call.use.physicals == std::set<PReg>{gpr(22), gpr(4), fpr(0)},
            "instruction uses must precede explicit and implicit definitions");
    require(call.def.physicals == defs, "CALL must merge explicit, implicit and clobber defs");
    require(exit.use.physicals == std::set<PReg>{fpr(24), fcc(0), gpr(1)},
            "RET implicit use and all physical classes must be included");
    require(exit.def.physicals == std::set<PReg>{fpr(24), gpr(4)}, "physical definitions");
    require(call.out.physicals == exit.in.physicals, "physical registers must cross edges");
    require(call.in.physicals == std::set<PReg>{gpr(22), gpr(4), fpr(0), fpr(24)},
            "CALL must kill successor caller-saved physical registers");
    require(exit.out.physicals.empty(), "no physical live after return");
}
static void intervals_same_slot() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "intervals_same_slot";
    auto x = f.new_vreg(ValueKind::I32), y = f.new_vreg(ValueKind::I32);
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(1)}},
                  {Opcode::COPY, {O::vreg(y, RegRole::Def), O::vreg(x)}},
                  {Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(y)}},
                  {Opcode::RET, {O::preg(gpr(4))}}},
                 {},
                 {}}};
    rebuild_cfg(f);
    auto a = analyze_checked(f);
    require_interval(a, x, 1, 3);
    require_interval(a, y, 2, 4);
    require(overlaps(a.intervals.at(x)->range, a.intervals.at(y)->range),
            "source and destination must overlap at their COPY slot");
}
static void intervals_order_and_multiple_defs() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "intervals_order_and_multiple_defs";
    auto x = f.new_vreg(ValueKind::I32), dead = f.new_vreg(ValueKind::F32);
    auto unused = f.new_vreg(ValueKind::I1);
    f.blocks = {{40,
                 "entry",
                 {{Opcode::BNEZ, {O::preg(gpr(4)), O::block(30)}}, {Opcode::B, {O::block(10)}}},
                 {},
                 {}},
                {90,
                 "join",
                 {{Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(x)}},
                  {Opcode::RET, {O::preg(gpr(4))}}},
                 {},
                 {}},
                {30,
                 "right",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(2)}},
                  {Opcode::B, {O::block(90)}}},
                 {},
                 {}},
                {10,
                 "left",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(1)}},
                  {Opcode::B, {O::block(90)}}},
                 {},
                 {}},
                {70, "unreachable_empty", {}, {}, {}},
                {5,
                 "unreachable_dead",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(dead, RegRole::Def), O::immediate(0)}}},
                 {},
                 {}}};
    rebuild_cfg(f);
    auto a = analyze_checked(f);
    require(a.order == std::vector<BlockId>{40, 30, 10, 90, 5, 70},
            "sorted successor RPO and sorted unreachable roots");
    const std::vector<Position> entries{0, 4, 8, 12, 16, 19};
    const std::vector<Position> exits{3, 7, 11, 15, 18, 20};
    for (size_t i = 0; i < a.order.size(); ++i) {
        const auto &block = a.blocks.at(a.order[i]);
        require(block.entry == entries[i] && block.exit == exits[i], "RPO block slots");
    }
    require_interval(a, x, 5, 14);
    require_interval(a, dead, 17, 18);
    require(!a.intervals.at(unused), "unreferenced vreg must have no interval");
    require(a.blocks.at(70).instructions.empty(), "empty block has no instruction slots");
}
static void intervals_empty_passthrough() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "intervals_empty_passthrough";
    auto x = f.new_vreg(ValueKind::Ptr64);
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(7)}},
                  {Opcode::B, {O::block(1)}}},
                 {},
                 {}},
                {1, "empty", {}, {}, {}},
                {2,
                 "exit",
                 {{Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(x)}},
                  {Opcode::RET, {O::preg(gpr(4))}}},
                 {},
                 {}}};
    rebuild_cfg(f);
    // An explicit CFG edge lets the analysis fixture model a block with zero instructions.
    f.block(1).successors = {2};
    f.block(2).predecessors = {1};
    auto a = analyze_checked(f);
    const auto &empty = a.blocks.at(1);
    require(empty.in.virtuals == std::set<VReg>{x} && empty.out.virtuals == std::set<VReg>{x},
            "empty block must pass live value through");
    require(empty.entry == 4 && empty.exit == 5 && empty.instructions.empty(),
            "empty block must have distinct entry and exit slots");
    require_interval(a, x, 1, 8);
}
static void liveness_nested_loop_multisweep() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "liveness_nested_loop_multisweep";
    auto x = f.new_vreg(ValueKind::I32);
    // Reverse storage order processes the inner header before the body that first reads x.
    // The inner/outer back edges therefore require another fixed-point sweep.
    f.blocks = {
        {0,
         "entry",
         {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(1)}},
          {Opcode::B, {O::block(10)}}},
         {},
         {}},
        {30,
         "inner_body",
         {{Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(x)}}, {Opcode::B, {O::block(20)}}},
         {},
         {}},
        {20,
         "inner_header",
         {{Opcode::BNEZ, {O::preg(gpr(4)), O::block(30)}}, {Opcode::B, {O::block(40)}}},
         {},
         {}},
        {10, "outer_header", {{Opcode::B, {O::block(20)}}}, {}, {}},
        {40,
         "outer_latch",
         {{Opcode::BNEZ, {O::preg(gpr(4)), O::block(10)}}, {Opcode::B, {O::block(50)}}},
         {},
         {}},
        {50, "exit", {{Opcode::RET, {O::preg(gpr(4))}}}, {}, {}}};
    rebuild_cfg(f);
    auto a = analyze_checked(f);
    for (BlockId id : {10u, 20u, 30u, 40u})
        require(a.blocks.at(id).in.virtuals == std::set<VReg>{x} &&
                    a.blocks.at(id).out.virtuals == std::set<VReg>{x},
                "nested back edges must reach liveness fixed point");
    require(a.blocks.at(0).in.virtuals.empty() && a.blocks.at(0).out.virtuals == std::set<VReg>{x},
            "entry definition must stop nested-loop input");
    require(a.order == std::vector<BlockId>{0, 10, 20, 40, 50, 30}, "nested-loop RPO");
    require_interval(a, x, 1, 22);
}
static void require_fixed(const LiveAnalysis &a, PReg reg, std::vector<Range> expected) {
    auto it = a.fixed.find(reg);
    require(it != a.fixed.end(), "expected fixed physical occupancy");
    require(it->second.size() == expected.size(), "unexpected fixed range count");
    for (size_t i = 0; i < expected.size(); ++i)
        require(it->second[i].begin == expected[i].begin && it->second[i].end == expected[i].end,
                "unexpected fixed physical range");
}
static void fixed_abi() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "fixed_abi";
    f.parameter_types = {ValueKind::I32, ValueKind::I32};
    auto x = f.new_vreg(ValueKind::I32), y = f.new_vreg(ValueKind::I32);
    auto z = f.new_vreg(ValueKind::I32);
    f.blocks = {
        {0,
         "entry",
         {{Opcode::COPY, {O::vreg(x, RegRole::Def), O::preg(gpr(4))}},
          {Opcode::COPY, {O::vreg(y, RegRole::Def), O::preg(gpr(5))}},
          {Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(x)}},
          {Opcode::COPY, {O::preg(gpr(5), RegRole::Def), O::vreg(y)}},
          {Opcode::CALL,
           {O::sym("callee"), O::preg(gpr(4)), O::preg(gpr(5)), O::preg(gpr(4), RegRole::Def)}},
          {Opcode::COPY, {O::vreg(z, RegRole::Def), O::preg(gpr(4))}},
          {Opcode::RET, {}}},
         {},
         {}}};
    rebuild_cfg(f);
    verify(MachineModule{{f}, {}}, VerifyStage::PreRA);
    auto a = analyze_checked(f);
    const auto &b = a.blocks.at(0);
    const auto &p = b.instructions;
    require_fixed(a, gpr(4), {{b.entry, p[0] + 1}, {p[2], p[5] + 1}});
    require_fixed(a, gpr(5), {{b.entry, p[1] + 1}, {p[3], p[4] + 1}});
    require_fixed(a, gpr(15), {{p[4], p[4] + 1}});
    require_fixed(a, gpr(1), {{p[4], p[6] + 1}});
    require(!a.fixed.count(gpr(23)), "CALL must not occupy unused callee-saved register");
    require(!b.use.physicals.count(gpr(15)) && !b.in.physicals.count(gpr(15)),
            "CALL clobber must not become entry use");
    require(!b.in.physicals.count(gpr(1)), "CALL implicit ra def must kill RET input");
    for (auto reg : caller_saved())
        require(a.fixed.count(reg) &&
                    std::any_of(a.fixed.at(reg).begin(), a.fixed.at(reg).end(),
                                [&](Range r) { return r.begin <= p[4] && p[4] < r.end; }),
                "all descriptor CALL clobbers must occupy CALL slot");
}
static void fixed_kills_and_usedef() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "fixed_kills_and_usedef";
    f.blocks = {
        {0,
         "entry",
         {{Opcode::LU32I_D, {O::preg(gpr(22), RegRole::UseDef), O::immediate(1)}},
          {Opcode::LOAD_CONSTANT, {O::preg(gpr(24), RegRole::Def), O::immediate(2)}},
          {Opcode::B, {O::block(1)}}},
         {},
         {}},
        {1,
         "exit",
         {{Opcode::COPY, {O::preg(gpr(25), RegRole::Def), O::preg(gpr(24))}}, {Opcode::RET, {}}},
         {},
         {}}};
    rebuild_cfg(f);
    verify(MachineModule{{f}, {}}, VerifyStage::PreRA);
    auto a = analyze_checked(f);
    const auto &entry = a.blocks.at(0), &exit = a.blocks.at(1);
    require(entry.use.physicals.count(gpr(22)) && entry.in.physicals.count(gpr(22)) &&
                entry.def.physicals.count(gpr(22)),
            "physical UseDef must read then kill");
    require(!entry.use.physicals.count(gpr(24)) && !entry.in.physicals.count(gpr(24)),
            "physical def must kill successor use");
    require(!entry.in.physicals.count(gpr(25)) && !exit.in.physicals.count(gpr(25)),
            "dead physical def must not become entry use");
    require_fixed(a, gpr(22), {{entry.entry, entry.instructions[0] + 1}});
    require_fixed(a, gpr(24), {{entry.instructions[1], exit.instructions[0] + 1}});
    require_fixed(a, gpr(25), {{exit.instructions[0], exit.instructions[0] + 1}});
    require_fixed(a, gpr(1), {{entry.entry, exit.instructions[1] + 1}});
}
static void fixed_call_result() {
    using O = MachineOperand;
    for (auto kind : {ValueKind::I32, ValueKind::F32}) {
        MachineFunction f;
        f.name = "fixed_call_result";
        auto x = f.new_vreg(kind);
        const auto result = kind == ValueKind::F32 ? fpr(0) : gpr(4);
        f.blocks = {{0,
                     "entry",
                     {{Opcode::CALL, {O::sym("callee"), O::preg(result, RegRole::Def)}},
                      {Opcode::COPY, {O::vreg(x, RegRole::Def), O::preg(result)}},
                      {Opcode::RET, {}}},
                     {},
                     {}}};
        rebuild_cfg(f);
        verify(MachineModule{{f}, {}}, VerifyStage::PreRA);
        auto a = analyze_checked(f);
        const auto &b = a.blocks.at(0);
        require(b.use.physicals.empty() && b.in.physicals.empty(),
                "CALL result and implicit/clobber defs must kill later uses at entry");
        require_fixed(a, result, {{b.instructions[0], b.instructions[1] + 1}});
        require_fixed(a, gpr(1), {{b.instructions[0], b.instructions[2] + 1}});
    }
}
static void target_registers() {
    const std::vector<PReg> expected_gprs = {gpr(15), gpr(16), gpr(17), gpr(18), gpr(19), gpr(4),
                                             gpr(5),  gpr(6),  gpr(7),  gpr(8),  gpr(9),  gpr(10),
                                             gpr(11), gpr(23), gpr(24), gpr(25), gpr(26), gpr(27),
                                             gpr(28), gpr(29), gpr(30), gpr(31)};
    const std::vector<PReg> expected_fprs = {
        fpr(11), fpr(12), fpr(13), fpr(14), fpr(15), fpr(16), fpr(17), fpr(18), fpr(19), fpr(20),
        fpr(21), fpr(22), fpr(23), fpr(0),  fpr(1),  fpr(2),  fpr(3),  fpr(4),  fpr(5),  fpr(6),
        fpr(7),  fpr(24), fpr(25), fpr(26), fpr(27), fpr(28), fpr(29), fpr(30), fpr(31)};
    require(allocatable_registers(RegClass::GPR) == expected_gprs, "GPR target priority order");
    require(allocatable_registers(RegClass::FPR) == expected_fprs, "FPR target priority order");
    require(allocatable_registers(RegClass::FCC).empty(), "FCC must not be allocatable");
    for (auto cls : {RegClass::GPR, RegClass::FPR, RegClass::FCC}) {
        const auto &regs = allocatable_registers(cls);
        require(std::set<PReg>(regs.begin(), regs.end()).size() == regs.size(),
                "allocatable registers must be unique");
        for (auto reg : regs)
            require(std::find(reserved_scratch().begin(), reserved_scratch().end(), reg) ==
                        reserved_scratch().end(),
                    "scratch must not be allocatable");
        for (unsigned i = 0; i <= 32; ++i)
            require(is_callee_saved({cls, i}) == (i <= 31 && ((cls == RegClass::GPR && i >= 23) ||
                                                              (cls == RegClass::FPR && i >= 24))),
                    "callee-saved classification boundaries");
    }
}
static void require_register(const AllocationResult &result, VReg value, PReg reg) {
    const auto &location = result.locations.at(value);
    require(location && std::holds_alternative<PReg>(*location) && std::get<PReg>(*location) == reg,
            "unexpected assigned register");
}
static void require_spill(const AllocationResult &result, VReg value, unsigned slot) {
    const auto &location = result.locations.at(value);
    require(location && std::holds_alternative<SpillLocation>(*location) &&
                std::get<SpillLocation>(*location).slot == slot,
            "unexpected final spill location");
}
static AllocationResult scan_checked(const MachineFunction &f, const LiveAnalysis &a,
                                     const RegisterSet &regs) {
    const auto before = dump(MachineModule{{f}, {}});
    auto result = linear_scan(f, a, regs), again = linear_scan(f, a, regs);
    verify_allocation(f, a, regs, result);
    verify_allocation(f, a, regs, again);
    require(before == dump(MachineModule{{f}, {}}), "scan must preserve function dump");
    require(result.locations.size() == f.vreg_types.size() &&
                result.locations.size() == again.locations.size(),
            "scan must retain complete VReg indexing");
    require(result.spill_frames.empty() && again.spill_frames.empty(),
            "scan must not materialize spill frames");
    for (VReg value = 0; value < result.locations.size(); ++value) {
        const auto &x = result.locations[value], &y = again.locations[value];
        require(x.has_value() == y.has_value(), "location presence must be deterministic");
        if (!x)
            continue;
        require(x->index() == y->index(), "location kind must be deterministic");
        if (std::holds_alternative<PReg>(*x))
            require_register(again, value, std::get<PReg>(*x));
        else
            require_spill(again, value, std::get<SpillLocation>(*x).slot);
    }
    require(result.spills.size() == again.spills.size() &&
                result.used_callee_saved == again.used_callee_saved &&
                result.stats.retained == again.stats.retained &&
                result.stats.spilled == again.stats.spilled,
            "allocation metadata must be deterministic");
    for (size_t i = 0; i < result.spills.size(); ++i) {
        const auto &x = result.spills[i], &y = again.spills[i];
        require(x.value == y.value && x.kind == y.kind && x.size == y.size &&
                    x.alignment == y.alignment,
                "spill metadata must be deterministic");
    }
    require(result.stats.reloads == 0 && result.stats.stores == 0 && again.stats.reloads == 0 &&
                again.stats.stores == 0,
            "pure scan must not emit spill traffic");
    return result;
}
static void scan_evict_longest() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "scan_evict_longest";
    f.vreg_types = {ValueKind::I32, ValueKind::I32, ValueKind::I32};
    f.blocks = {
        {0,
         "entry",
         {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(7)}}, {Opcode::RET, {}}},
         {},
         {}}};
    LiveAnalysis a;
    a.intervals = {LiveInterval{0, {0, 20}}, LiveInterval{1, {1, 6}}, LiveInterval{2, {2, 8}}};
    auto result = scan_checked(f, a, {{gpr(15), gpr(16)}, {}});
    const auto &longest = result.locations.at(0);
    require(longest && std::holds_alternative<SpillLocation>(*longest), "spill longest");
    require_spill(result, 0, 0);
    require_register(result, 1, gpr(16));
    require_register(result, 2, gpr(15));
    require(result.stats.retained == 3 && result.stats.spilled == 1, "final eviction stats");
    require(f.frame_objects.empty(), "decision must not create frame objects");
}
static void scan_boundaries_and_equal_end() {
    MachineFunction f;
    f.vreg_types = {ValueKind::I32, ValueKind::I32};
    LiveAnalysis a;
    a.intervals = {LiveInterval{0, {0, 6}}, LiveInterval{1, {6, 9}}};
    auto adjacent = scan_checked(f, a, {{gpr(15)}, {}});
    require_register(adjacent, 0, gpr(15));
    require_register(adjacent, 1, gpr(15));
    require(adjacent.spills.empty(), "half-open endpoint must release register");
    a.intervals.at(1) = LiveInterval{1, {1, 6}};
    auto tied = scan_checked(f, a, {{gpr(15)}, {}});
    require_register(tied, 0, gpr(15));
    require_spill(tied, 1, 0);
}
static void scan_order_and_victim_ties() {
    MachineFunction f;
    f.vreg_types = {ValueKind::I32, ValueKind::I32, ValueKind::I32};
    LiveAnalysis a;
    a.intervals = {LiveInterval{0, {0, 10}}, LiveInterval{1, {0, 5}}, LiveInterval{2, {0, 5}}};
    auto ordered = scan_checked(f, a, {{gpr(15), gpr(16)}, {}});
    require_spill(ordered, 0, 0);
    require_register(ordered, 1, gpr(15));
    require_register(ordered, 2, gpr(16));
    for (Position second_begin : {0u, 1u}) {
        a.intervals = {LiveInterval{0, {0, 10}}, LiveInterval{1, {second_begin, 10}},
                       LiveInterval{2, {2, 5}}};
        auto victim = scan_checked(f, a, {{gpr(15), gpr(16)}, {}});
        require_register(victim, 0, gpr(15));
        require_spill(victim, 1, 0);
        require_register(victim, 2, gpr(16));
    }
}
static void scan_empty_and_spill_metadata() {
    MachineFunction f;
    f.vreg_types = {ValueKind::I1, ValueKind::Ptr64, ValueKind::I32, ValueKind::F32,
                    ValueKind::I32};
    f.frame_objects = {{ValueKind::Ptr64, 24, 8, FramePurpose::Alloca, -32}};
    LiveAnalysis a;
    a.intervals = {LiveInterval{0, {8, 9}}, LiveInterval{1, {4, 5}}, LiveInterval{2, {0, 1}},
                   LiveInterval{3, {2, 3}}, std::nullopt};
    auto result = scan_checked(f, a, {});
    require(result.spills.size() == 4 && result.stats.retained == 4 && result.stats.spilled == 4,
            "empty register set must spill exactly appeared values");
    for (VReg value = 0; value < 4; ++value) {
        require_spill(result, value, value);
        const auto &slot = result.spills.at(value);
        const unsigned size = value == 1 ? 8 : 4;
        require(slot.value == value && slot.kind == f.vreg_types.at(value) && slot.size == size &&
                    slot.alignment == size,
                "spill slots must follow VReg order and kind width/alignment");
    }
    require(!result.locations.at(4), "unseen VReg must have no location");
    require(result.used_callee_saved.empty(), "all spill must not use callee-saved registers");
    require(f.frame_objects.size() == 1 && f.frame_objects.at(0).offset == -32,
            "scan must preserve existing alloca");
    MachineFunction empty;
    auto no_values = scan_checked(empty, {}, default_registers());
    require(no_values.locations.empty() && no_values.spills.empty() &&
                no_values.used_callee_saved.empty() && no_values.stats.retained == 0 &&
                no_values.stats.spilled == 0,
            "empty function must have empty allocation");
}
static void scan_classes_and_callee_saved() {
    MachineFunction f;
    f.vreg_types = {ValueKind::F32,   ValueKind::I32, ValueKind::F32,
                    ValueKind::Ptr64, ValueKind::I1,  ValueKind::F32};
    LiveAnalysis a;
    a.intervals = {LiveInterval{0, {0, 4}}, LiveInterval{1, {0, 4}}, LiveInterval{2, {1, 3}},
                   LiveInterval{3, {1, 3}}, LiveInterval{4, {4, 5}}, LiveInterval{5, {4, 5}}};
    auto result = scan_checked(f, a, {{gpr(23), gpr(24)}, {fpr(24), fpr(25)}});
    require_register(result, 0, fpr(24));
    require_register(result, 1, gpr(23));
    require_register(result, 2, fpr(25));
    require_register(result, 3, gpr(24));
    require_register(result, 4, gpr(23));
    require_register(result, 5, fpr(24));
    require(result.spills.empty() && result.stats.retained == 6 && result.stats.spilled == 0,
            "GPR and FPR banks must allocate independently");
    require(result.used_callee_saved == std::vector<PReg>{gpr(23), gpr(24), fpr(24), fpr(25)},
            "used callee-saved registers must be deduplicated and sorted");
    auto defaults = default_registers();
    require(defaults.gprs == allocatable_registers(RegClass::GPR) &&
                defaults.fprs == allocatable_registers(RegClass::FPR),
            "default registers must wrap target priority sets");
}
static void scan_fixed_constraints() {
    MachineFunction f;
    f.vreg_types = {ValueKind::I32};
    LiveAnalysis a;
    a.intervals = {LiveInterval{0, {2, 10}}};
    a.fixed[gpr(15)] = {{4, 5}};
    auto middle = scan_checked(f, a, {{gpr(15), gpr(16)}, {}});
    require_register(middle, 0, gpr(16));
    a.fixed[gpr(15)] = {{0, 2}, {10, 12}};
    auto touching = scan_checked(f, a, {{gpr(15), gpr(16)}, {}});
    require_register(touching, 0, gpr(15));
    a.fixed[gpr(15)] = {{4, 5}};
    auto none_legal = scan_checked(f, a, {{gpr(15)}, {}});
    require_spill(none_legal, 0, 0);
    f.vreg_types.push_back(ValueKind::I32);
    a.intervals = {LiveInterval{0, {0, 3}}, LiveInterval{1, {1, 6}}};
    a.fixed[gpr(16)] = {{0, 20}};
    auto underfull = scan_checked(f, a, {{gpr(15), gpr(16)}, {}});
    require_register(underfull, 0, gpr(15));
    require_spill(underfull, 1, 0);
    f.vreg_types.push_back(ValueKind::I32);
    a.intervals = {LiveInterval{0, {0, 10}}, LiveInterval{1, {1, 6}}, LiveInterval{2, {2, 12}}};
    a.fixed = {{gpr(15), {{11, 12}}}};
    auto incompatible = scan_checked(f, a, {{gpr(15), gpr(16)}, {}});
    require_register(incompatible, 0, gpr(15));
    require_register(incompatible, 1, gpr(16));
    require_spill(incompatible, 2, 0);
}
static void rejected(const std::function<void()> &fn, const std::vector<std::string> &context) {
    bool failed = false;
    try {
        fn();
    } catch (const std::runtime_error &e) {
        failed = true;
        for (const auto &part : context)
            require(std::string(e.what()).find(part) != std::string::npos,
                    "allocation diagnostic must identify involved function/value/register");
    }
    require(failed, "expected register allocation rejection");
}
static void verify_mutations() {
    MachineFunction f;
    f.name = "verify_mutations";
    f.vreg_types = {ValueKind::I32, ValueKind::I32, ValueKind::I32, ValueKind::I32};
    LiveAnalysis a;
    a.intervals = {LiveInterval{0, {0, 20}}, LiveInterval{1, {1, 6}}, LiveInterval{2, {2, 8}},
                   std::nullopt};
    RegisterSet regs{{gpr(15), gpr(16)}, {}};
    auto result = linear_scan(f, a, regs);
    verify_allocation(f, a, regs, result);
    auto bad = result;
    bad.locations.at(1) = gpr(15);
    bad.locations.at(2) = gpr(15);
    rejected([&] { verify_allocation(f, a, regs, bad); },
             {f.name, "v1", "v2", register_name(gpr(15))});
    bad = result;
    bad.locations.at(1) = gpr(12);
    // Even a supplied bank containing scratch must not authorize a reserved register.
    auto scratch_regs = regs;
    scratch_regs.gprs.push_back(gpr(12));
    rejected([&] { verify_allocation(f, a, scratch_regs, bad); },
             {f.name, "v1", register_name(gpr(12))});
    bad = result;
    bad.locations.at(1) = fpr(11);
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v1", register_name(fpr(11))});
    bad = result;
    bad.locations.at(1).reset();
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v1"});
    bad = result;
    bad.locations.pop_back();
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name});
    bad = result;
    bad.locations.at(3) = gpr(15);
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v3"});
    bad = result;
    bad.locations.at(1) = gpr(17);
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v1", register_name(gpr(17))});
    // A legal allocation may use a different register priority and spill more values.
    auto alternative = result;
    alternative.locations.at(1) = gpr(15);
    alternative.locations.at(2) = gpr(16);
    verify_allocation(f, a, regs, alternative);
    auto spilled = linear_scan(f, a, {});
    verify_allocation(f, a, regs, spilled);
    bad = spilled;
    bad.locations.at(2) = bad.locations.at(1);
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v2"});
    bad = spilled;
    bad.locations.at(0) = SpillLocation{99};
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v0"});
    bad = spilled;
    bad.spills.at(0).value = 1;
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v0", "v1"});
    bad = spilled;
    bad.spills.at(0).kind = ValueKind::F32;
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v0"});
    bad = spilled;
    bad.spills.push_back(spilled.spills.at(0));
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v0"});
}
static void verify_spill_widths() {
    MachineFunction f;
    f.name = "verify_spill_widths";
    f.vreg_types = {ValueKind::I1, ValueKind::I32, ValueKind::Ptr64, ValueKind::F32};
    LiveAnalysis a;
    for (VReg value = 0; value < f.vreg_types.size(); ++value)
        a.intervals.push_back(LiveInterval{value, {1, 2}});
    auto result = linear_scan(f, a, {});
    verify_allocation(f, a, {}, result);
    for (VReg value = 0; value < f.vreg_types.size(); ++value) {
        auto bad = result;
        bad.spills.at(value).size = value == 2 ? 4 : 8;
        rejected([&] { verify_allocation(f, a, {}, bad); }, {f.name, "v" + std::to_string(value)});
        bad = result;
        bad.spills.at(value).alignment = 1;
        rejected([&] { verify_allocation(f, a, {}, bad); }, {f.name, "v" + std::to_string(value)});
    }
    verify_allocation(MachineFunction{}, LiveAnalysis{}, default_registers(), AllocationResult{});
}
static void verify_call_and_saved() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "verify_call_and_saved";
    f.parameter_types = {ValueKind::F32};
    auto x = f.new_vreg(ValueKind::I32), y = f.new_vreg(ValueKind::F32);
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(x, RegRole::Def), O::immediate(1)}},
                  {Opcode::COPY, {O::vreg(y, RegRole::Def), O::preg(fpr(0))}},
                  {Opcode::CALL, {O::sym("callee")}},
                  {Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(x)}},
                  {Opcode::COPY, {O::preg(fpr(0), RegRole::Def), O::vreg(y)}},
                  {Opcode::RET, {}}},
                 {},
                 {}}};
    rebuild_cfg(f);
    verify(MachineModule{{f}, {}}, VerifyStage::PreRA);
    auto a = analyze_liveness(f);
    auto regs = default_registers();
    auto result = linear_scan(f, a, regs);
    require_register(result, x, gpr(23));
    require_register(result, y, fpr(24));
    verify_allocation(f, a, regs, result);
    for (auto pair : {std::make_pair(x, gpr(15)), std::make_pair(y, fpr(11))}) {
        auto bad = result;
        bad.locations.at(pair.first) = pair.second;
        rejected([&] { verify_allocation(f, a, regs, bad); },
                 {f.name, "v" + std::to_string(pair.first), register_name(pair.second)});
    }
    auto bad = result;
    bad.used_callee_saved.clear();
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, "v0", register_name(gpr(23))});
    bad = result;
    bad.used_callee_saved.push_back(gpr(24));
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, register_name(gpr(24))});
    bad = result;
    bad.used_callee_saved.push_back(gpr(23));
    rejected([&] { verify_allocation(f, a, regs, bad); }, {f.name, register_name(gpr(23))});
    // Touching endpoints are legal for both virtual and fixed intervals.
    MachineFunction touching;
    touching.name = "verify_touching";
    touching.vreg_types = {ValueKind::I32, ValueKind::I32};
    LiveAnalysis boundaries;
    boundaries.intervals = {LiveInterval{0, {2, 4}}, LiveInterval{1, {4, 6}}};
    boundaries.fixed[gpr(15)] = {{0, 2}, {6, 7}};
    RegisterSet one{{gpr(15)}, {}};
    verify_allocation(touching, boundaries, one, linear_scan(touching, boundaries, one));
}

static AllocationResult rewrite_checked(MachineFunction &f, const RegisterSet &regs = {}) {
    rebuild_cfg(f);
    verify(MachineModule{{f}, {}}, VerifyStage::PreRA);
    auto a = analyze_liveness(f);
    auto result = linear_scan(f, a, regs);
    verify_allocation(f, a, regs, result);
    rewrite_allocation(f, result);
    verify(MachineModule{{f}, {}}, VerifyStage::Allocated);
    for (const auto &block : f.blocks)
        for (const auto &i : block.instructions)
            for (const auto &o : i.operands)
                require(o.kind != OperandKind::VReg, "rewrite must remove every VReg");
    require(f.used_callee_saved == result.used_callee_saved, "copy final saved-register metadata");
    return result;
}
static void require_memory(const MachineInstr &i, Opcode opcode, PReg reg, unsigned frame) {
    require(i.opcode == opcode, "wrong spill/reload opcode");
    require(i.operands.size() == 3 && i.operands[0].kind == OperandKind::PReg &&
                i.operands[0].reg == reg &&
                i.operands[0].role == (descriptor(opcode).may_load ? RegRole::Def : RegRole::Use),
            "wrong spill/reload register or role");
    require(i.operands[1].kind == OperandKind::FrameIndex && i.operands[1].id == frame &&
                i.operands[2].kind == OperandKind::Immediate && i.operands[2].imm == 0,
            "wrong spill/reload frame or displacement");
}
static void rewrite_repeated_and_usedef() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "rewrite_repeated";
    f.vreg_types = {ValueKind::I32, ValueKind::I32, ValueKind::I32};
    f.frame_objects = {{ValueKind::Ptr64, 24, 8, FramePurpose::Alloca, 17}};
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(7)}},
                  {Opcode::ADD_W, {O::vreg(1, RegRole::Def), O::vreg(0), O::vreg(0)}},
                  {Opcode::RET, {}}},
                 {},
                 {}}};
    auto r = rewrite_checked(f);
    require(f.frame_objects.size() == 3 && r.spill_frames == std::vector<unsigned>{1, 2},
            "create only actual spill homes, following spill order");
    require(!r.locations.at(2), "unseen VReg must not acquire a home");
    const auto &old = f.frame_objects.at(0);
    require(old.kind == ValueKind::Ptr64 && old.size == 24 && old.alignment == 8 &&
                old.purpose == FramePurpose::Alloca && old.offset == 17,
            "preserve existing alloca");
    const auto &is = f.block(0).instructions;
    require(is.size() == 6, "repeated use must reload exactly once");
    require_memory(is.at(1), Opcode::ST_W, gpr(12), r.spill_frames.at(0));
    require_memory(is.at(2), Opcode::LD_W, gpr(12), r.spill_frames.at(0));
    require(is.at(3).opcode == Opcode::ADD_W && is.at(3).operands[1].reg == gpr(12) &&
                is.at(3).operands[2].reg == gpr(12),
            "both repeated inputs use one scratch");
    require_memory(is.at(4), Opcode::ST_W, is.at(3).operands[0].reg, r.spill_frames.at(1));
    require(r.stats.reloads == 1 && r.stats.stores == 2, "repeated source traffic stats");

    MachineFunction u;
    u.name = "rewrite_usedef";
    u.vreg_types = {ValueKind::Ptr64};
    u.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(9)}},
                  {Opcode::LU32I_D, {O::vreg(0, RegRole::UseDef), O::immediate(1)}},
                  {Opcode::RET, {}}},
                 {},
                 {}}};
    auto ur = rewrite_checked(u);
    const auto &ui = u.block(0).instructions;
    require(ui.size() == 6 && ui.at(3).opcode == Opcode::LU32I_D &&
                ui.at(3).operands[0].role == RegRole::UseDef,
            "preserve read/write instruction");
    auto scratch = ui.at(3).operands[0].reg;
    require_memory(ui.at(1), Opcode::ST_D, scratch, ur.spill_frames.at(0));
    require_memory(ui.at(2), Opcode::LD_D, scratch, ur.spill_frames.at(0));
    require_memory(ui.at(4), Opcode::ST_D, scratch, ur.spill_frames.at(0));
    require(ur.stats.reloads == 1 && ur.stats.stores == 2, "UseDef traffic stats");
}
static void rewrite_sources_and_widths() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "rewrite_sources";
    f.vreg_types = {ValueKind::I32, ValueKind::I32, ValueKind::I32};
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(2)}},
                  {Opcode::LOAD_CONSTANT, {O::vreg(1, RegRole::Def), O::immediate(3)}},
                  {Opcode::SUB_W, {O::vreg(2, RegRole::Def), O::vreg(0), O::vreg(1)}},
                  {Opcode::RET, {}}},
                 {},
                 {}}};
    auto r = rewrite_checked(f);
    const auto &is = f.block(0).instructions;
    require(is.size() == 9, "two-source rewrite length");
    require_memory(is.at(4), Opcode::LD_W, gpr(12), r.spill_frames.at(0));
    require_memory(is.at(5), Opcode::LD_W, gpr(13), r.spill_frames.at(1));
    require(is.at(6).opcode == Opcode::SUB_W && is.at(6).operands[1].reg == gpr(12) &&
                is.at(6).operands[2].reg == gpr(13),
            "distinct inputs loaded before operation");
    require_memory(is.at(7), Opcode::ST_W, is.at(6).operands[0].reg, r.spill_frames.at(2));

    MachineFunction widths;
    widths.name = "rewrite_widths";
    widths.vreg_types = {ValueKind::I1, ValueKind::I32, ValueKind::Ptr64, ValueKind::F32};
    widths.frame_objects = {{ValueKind::F32, 4, 4, FramePurpose::Alloca}};
    widths.blocks = {{0,
                      "entry",
                      {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(1)}},
                       {Opcode::LOAD_CONSTANT, {O::vreg(1, RegRole::Def), O::immediate(-2)}},
                       {Opcode::LOAD_CONSTANT, {O::vreg(2, RegRole::Def), O::immediate(9)}},
                       {Opcode::FLD_S, {O::vreg(3, RegRole::Def), O::frame(0), O::immediate(0)}},
                       {Opcode::ST_B, {O::vreg(0), O::frame(0), O::immediate(0)}},
                       {Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(0)}},
                       {Opcode::COPY, {O::preg(gpr(5), RegRole::Def), O::vreg(1)}},
                       {Opcode::COPY, {O::preg(gpr(6), RegRole::Def), O::vreg(2)}},
                       {Opcode::COPY, {O::preg(fpr(0), RegRole::Def), O::vreg(3)}},
                       {Opcode::RET, {}}},
                      {},
                      {}}};
    auto wr = rewrite_checked(widths);
    const auto &wi = widths.block(0).instructions;
    const Opcode stores[] = {Opcode::ST_W, Opcode::ST_W, Opcode::ST_D, Opcode::FST_S};
    const Opcode loads[] = {Opcode::LD_W, Opcode::LD_W, Opcode::LD_D, Opcode::FLD_S};
    for (unsigned v = 0; v < 4; ++v) {
        const auto &frame = widths.frame_objects.at(wr.spill_frames.at(v));
        unsigned size = v == 2 ? 8 : 4;
        require(frame.purpose == FramePurpose::Spill && frame.kind == widths.vreg_types[v] &&
                    frame.size == size && frame.alignment == size,
                "all four spill slot widths");
        require_memory(wi.at(2 * v + 1), stores[v], v == 3 ? fpr(8) : gpr(12), wr.spill_frames[v]);
        require_memory(wi.at(10 + v), loads[v], v == 3 ? fpr(0) : gpr(4 + v), wr.spill_frames[v]);
    }
    require(wi.at(6).opcode == Opcode::FLD_S && wi.at(9).opcode == Opcode::ST_B,
            "original ordinary memory instructions are preserved");
    require(wr.stats.reloads == 5 && wr.stats.stores == 4,
            "stats count inserted memory only, including byte-use word reload");
}
static void rewrite_copies() {
    using O = MachineOperand;
    for (unsigned mask = 0; mask < 4; ++mask) {
        MachineFunction f;
        f.name = "rewrite_copy_" + std::to_string(mask);
        f.vreg_types = {ValueKind::I32, ValueKind::I32};
        f.blocks = {{0,
                     "entry",
                     {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(11)}},
                      {Opcode::COPY, {O::vreg(1, RegRole::Def), O::vreg(0)}},
                      {Opcode::RET, {}}},
                     {},
                     {}}};
        rebuild_cfg(f);
        verify(MachineModule{{f}, {}}, VerifyStage::PreRA);
        auto a = analyze_liveness(f);
        auto r = linear_scan(f, a, {});
        // Hand-select all four legal location combinations. The COPY slot overlaps both values.
        r.spills.clear();
        r.used_callee_saved.clear();
        for (VReg v = 0; v < 2; ++v) {
            if (mask & (1u << v)) {
                r.locations[v] = SpillLocation{static_cast<unsigned>(r.spills.size())};
                r.spills.push_back({v, ValueKind::I32, 4, 4});
            } else
                r.locations[v] = gpr(15 + v);
        }
        verify_allocation(f, a, {{gpr(15), gpr(16)}, {}}, r);
        rewrite_allocation(f, r);
        verify(MachineModule{{f}, {}}, VerifyStage::Allocated);
        const auto &is = f.block(0).instructions;
        unsigned n = (mask & 1) ? 2 : 1;
        if (mask == 0) {
            require(is.at(n).opcode == Opcode::COPY && is.at(n).operands[0].reg == gpr(16) &&
                        is.at(n).operands[1].reg == gpr(15),
                    "register COPY must survive");
            require(r.stats.reloads == 0 && r.stats.stores == 0, "reg/reg COPY traffic");
        } else if (mask == 1) {
            require_memory(is.at(n), Opcode::LD_W, gpr(16), r.spill_frames.at(0));
            require(r.stats.reloads == 1 && r.stats.stores == 1, "stack/reg COPY traffic");
        } else if (mask == 2) {
            require_memory(is.at(n), Opcode::ST_W, gpr(15), r.spill_frames.at(0));
            require(r.stats.reloads == 0 && r.stats.stores == 1, "reg/stack COPY traffic");
        } else {
            require_memory(is.at(n), Opcode::LD_W, gpr(12), r.spill_frames.at(0));
            require_memory(is.at(n + 1), Opcode::ST_W, gpr(12), r.spill_frames.at(1));
            require(r.stats.reloads == 1 && r.stats.stores == 2, "stack/stack COPY traffic");
        }
        require(is.size() == n + (mask == 3 ? 2 : 1) + 1, "COPY emits only required traffic");
    }
    MachineFunction same;
    same.name = "rewrite_same_register_copy";
    same.blocks = {
        {0,
         "entry",
         {{Opcode::COPY, {O::preg(gpr(0), RegRole::Def), O::preg(gpr(0))}}, {Opcode::RET, {}}},
         {},
         {}}};
    auto r = rewrite_checked(same);
    require(same.block(0).instructions.size() == 1 && r.spill_frames.empty(),
            "remove physical same-register COPY without homes");
}
static void rewrite_branch_and_call() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "rewrite_branch";
    f.vreg_types = {ValueKind::I1};
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(1)}},
                  {Opcode::BNEZ, {O::vreg(0), O::block(1)}},
                  {Opcode::B, {O::block(2)}}},
                 {},
                 {}},
                {1, "yes", {{Opcode::RET, {}}}, {}, {}},
                {2, "no", {{Opcode::RET, {}}}, {}, {}}};
    auto r = rewrite_checked(f);
    const auto &is = f.block(0).instructions;
    require_memory(is.at(2), Opcode::LD_W, gpr(12), r.spill_frames.at(0));
    require(is.size() == 5 && is.at(3).opcode == Opcode::BNEZ &&
                is.at(3).operands[0].reg == gpr(12) && is.at(4).opcode == Opcode::B,
            "branch reload precedes conditional and leaves false branch adjacent");
    MachineFunction call;
    call.name = "rewrite_call_saved";
    call.vreg_types = {ValueKind::I32, ValueKind::F32};
    call.parameter_types = {ValueKind::F32};
    call.blocks = {{0,
                    "entry",
                    {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(1)}},
                     {Opcode::COPY, {O::vreg(1, RegRole::Def), O::preg(fpr(0))}},
                     {Opcode::CALL, {O::sym("callee")}},
                     {Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(0)}},
                     {Opcode::COPY, {O::preg(fpr(0), RegRole::Def), O::vreg(1)}},
                     {Opcode::RET, {}}},
                    {},
                    {}}};
    auto cr = rewrite_checked(call, default_registers());
    require(call.used_callee_saved == std::vector<PReg>{gpr(23), fpr(24)} &&
                cr.stats.reloads == 0 && cr.stats.stores == 0 &&
                call.block(0).instructions.at(2).opcode == Opcode::CALL,
            "physical CALL stays in place and final saved metadata is copied");
}
// Execute the lowered loop twice, including its PHI backedge, to observe the rotation.
static std::vector<int64_t> run_swap(const MachineFunction &f, unsigned count, BlockId backedge) {
    std::map<PReg, int64_t> regs;
    std::map<unsigned, int64_t> frames;
    for (auto block : {BlockId{0}, BlockId{1}, backedge, BlockId{1}})
        for (const auto &i : f.block(block).instructions) {
            if (i.opcode == Opcode::LOAD_CONSTANT)
                regs[i.operands[0].reg] = i.operands[1].imm;
            else if (i.opcode == Opcode::COPY)
                regs[i.operands[0].reg] = regs.at(i.operands[1].reg);
            else if (i.opcode == Opcode::LD_W)
                regs[i.operands[0].reg] = frames.at(i.operands[1].id);
            else if (i.opcode == Opcode::ST_W)
                frames[i.operands[1].id] = regs.at(i.operands[0].reg);
            else
                require(i.opcode == Opcode::B || i.opcode == Opcode::BNEZ,
                        "unexpected swap opcode");
        }
    std::vector<int64_t> values;
    for (unsigned v = 0; v < count; ++v)
        values.push_back(regs.at(gpr(4 + v)));
    return values;
}
static void rewrite_phi_swaps() {
    using O = MachineOperand;
    for (unsigned count : {2u, 3u}) {
        for (bool spilled : {false, true}) {
            MachineFunction selected;
            selected.name = "rewrite_phi_swap_" + std::to_string(count);
            selected.blocks = {{0, "entry", {}, {}, {}},
                               {1, "loop", {}, {}, {}},
                               {2, "exit", {{Opcode::RET, {}}}, {}, {}}};
            for (unsigned v = 0; v < count * 2; ++v)
                selected.new_vreg(ValueKind::I32);
            auto condition = selected.new_vreg(ValueKind::I1);
            for (unsigned v = 0; v < count; ++v) {
                selected.block(0).instructions.push_back(
                    {Opcode::LOAD_CONSTANT,
                     {O::vreg(v, RegRole::Def), O::immediate(10 * (v + 1))}});
                selected.block(1).instructions.push_back(
                    {Opcode::PHI,
                     {O::vreg(count + v, RegRole::Def), O::vreg(v), O::block(0),
                      O::vreg(count + (v + 1) % count), O::block(1)}});
            }
            selected.block(0).instructions.push_back(
                {Opcode::LOAD_CONSTANT, {O::vreg(condition, RegRole::Def), O::immediate(1)}});
            selected.block(0).instructions.push_back({Opcode::B, {O::block(1)}});
            for (unsigned v = 0; v < count; ++v)
                selected.block(1).instructions.push_back(
                    {Opcode::COPY, {O::preg(gpr(4 + v), RegRole::Def), O::vreg(count + v)}});
            selected.block(1).instructions.push_back(
                {Opcode::BNEZ, {O::vreg(condition), O::block(1)}});
            selected.block(1).instructions.push_back({Opcode::B, {O::block(2)}});
            rebuild_cfg(selected);
            MachineModule module{{selected}, {}};
            verify(module, VerifyStage::Selected);
            lower(module, {});
            auto &f = module.functions.at(0);
            // The two-successor loop edge is split for the PHI cycle.
            auto backedge =
                f.block(1).instructions.at(f.block(1).instructions.size() - 2).operands.back().id;
            require(backedge != 1 && backedge != 2, "PHI backedge is split");
            auto r = rewrite_checked(f, spilled ? RegisterSet{} : default_registers());
            std::vector<int64_t> expected;
            for (unsigned v = 0; v < count; ++v)
                expected.push_back(10 * ((v + 1) % count + 1));
            require(run_swap(f, count, backedge) == expected,
                    "PHI cycle must preserve all old values across the real lowered backedge");
            require(f.vreg_types.size() == count * 2 + 2, "PHI cycle introduces one temporary");
            require(spilled ? r.spills.size() == f.vreg_types.size() : r.spills.empty(),
                    "exercise both fully spilled and fully resident PHI cycles");
        }
    }
}
static void rewrite_scratch_conflicts() {
    using O = MachineOperand;
    for (auto reg : {gpr(12), fpr(8)}) {
        for (bool live_through : {false, true}) {
            MachineFunction f;
            f.name = "rewrite_scratch_conflict";
            auto kind = reg.cls == RegClass::GPR ? ValueKind::I32 : ValueKind::F32;
            f.vreg_types = {kind, kind};
            f.frame_objects = {{kind, 4, 4, FramePurpose::Alloca}};
            auto load = reg.cls == RegClass::GPR ? Opcode::LD_W : Opcode::FLD_S;
            f.blocks = {{0,
                         "entry",
                         {{load, {O::preg(reg, RegRole::Def), O::frame(0), O::immediate(0)}},
                          {load, {O::vreg(0, RegRole::Def), O::frame(0), O::immediate(0)}},
                          {reg.cls == RegClass::GPR ? Opcode::ADD_W : Opcode::FADD_S,
                           {O::vreg(1, RegRole::Def), O::vreg(0),
                            live_through ? O::vreg(0) : O::preg(reg)}},
                          {Opcode::COPY,
                           {O::preg(reg.cls == RegClass::GPR ? gpr(4) : fpr(0), RegRole::Def),
                            O::preg(reg)}},
                          {Opcode::RET, {}}},
                         {},
                         {}}};
            if (!live_through)
                std::swap(f.block(0).instructions[0], f.block(0).instructions[1]);
            rebuild_cfg(f);
            verify(MachineModule{{f}, {}}, VerifyStage::PreRA);
            auto a = analyze_liveness(f);
            auto r = linear_scan(f, a, {});
            verify_allocation(f, a, {}, r);
            rejected([&] { rewrite_allocation(f, r); },
                     {f.name, "bb0", live_through ? "instruction 1" : "instruction 2",
                      register_name(reg)});
        }
    }
    MachineFunction implicit;
    implicit.name = "rewrite_implicit_call";
    implicit.blocks = {
        {0, "entry", {{Opcode::CALL, {O::sym("callee")}}, {Opcode::RET, {}}}, {}, {}}};
    auto r = rewrite_checked(implicit);
    require(r.stats.reloads == 0 && r.stats.stores == 0, "implicit CALL clobbers need no scratch");
}
static void allocated_verifier() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "allocated_verifier";
    f.vreg_types = {ValueKind::Ptr64};
    f.frame_objects = {{ValueKind::Ptr64, 8, 8, FramePurpose::Spill}};
    f.blocks = {{0,
                 "entry",
                 {{Opcode::LOAD_CONSTANT, {O::preg(gpr(12), RegRole::Def), O::immediate(9)}},
                  {Opcode::LU32I_D, {O::preg(gpr(12), RegRole::UseDef), O::immediate(1)}},
                  {Opcode::SYMBOL_ADDR, {O::preg(gpr(13), RegRole::Def), O::sym("object")}},
                  {Opcode::FRAME_ADDR, {O::preg(gpr(14), RegRole::Def), O::frame(0)}},
                  {Opcode::LD_D, {O::preg(gpr(12), RegRole::Def), O::frame(0), O::immediate(0)}},
                  {Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::preg(gpr(12))}},
                  {Opcode::RET, {}}},
                 {},
                 {}}};
    rebuild_cfg(f);
    verify(MachineModule{{f}, {}}, VerifyStage::Allocated);
    auto bad = f;
    bad.block(0).instructions[0].operands[0] = O::vreg(0, RegRole::Def);
    rejected([&] { verify(MachineModule{{bad}, {}}, VerifyStage::Allocated); },
             {f.name, "virtual register"});
    bad = f;
    bad.block(0).instructions[3].operands[1] = O::frame(1);
    rejected([&] { verify(MachineModule{{bad}, {}}, VerifyStage::Allocated); },
             {f.name, "FrameIndex"});
    for (auto opcode : {Opcode::ARG, Opcode::PHI, Opcode::HIGH_CALL, Opcode::RETURN}) {
        bad = f;
        std::vector<O> operands;
        if (opcode == Opcode::ARG)
            operands = {O::vreg(0, RegRole::Def), O::immediate(0)};
        if (opcode == Opcode::PHI)
            operands = {O::vreg(0, RegRole::Def), O::vreg(0), O::block(0)};
        if (opcode == Opcode::HIGH_CALL)
            operands = {O::sym("callee")};
        bad.block(0).instructions.insert(bad.block(0).instructions.begin(), {opcode, operands});
        rejected([&] { verify(MachineModule{{bad}, {}}, VerifyStage::Allocated); },
                 {f.name, "high-level pseudo"});
    }
    // Allocated checks structure, not PreRA physical must-definition availability.
    auto physical = f;
    physical.block(0).instructions = {
        {Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::preg(gpr(15))}}, {Opcode::RET, {}}};
    verify(MachineModule{{physical}, {}}, VerifyStage::Allocated);
    rejected([&] { verify(MachineModule{{physical}, {}}, VerifyStage::PreRA); }, {f.name});
    rejected([&] { verify(MachineModule{{f}, {}}, VerifyStage::Final); },
             {f.name, "unexpanded pseudo"});
    auto malformed = physical;
    malformed.block(0).instructions[0].operands[1] = O::preg(fpr(0));
    rejected([&] { verify(MachineModule{{malformed}, {}}, VerifyStage::Allocated); },
             {f.name, "class"});
    malformed = physical;
    malformed.block(0).successors = {0};
    rejected([&] { verify(MachineModule{{malformed}, {}}, VerifyStage::Allocated); },
             {f.name, "CFG"});
}

static MachineFunction saved_frame_fixture(unsigned bytes = 24) {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "frame_saved_" + std::to_string(bytes);
    f.parameter_types = {ValueKind::I32};
    f.used_callee_saved = {gpr(23), fpr(24)};
    f.frame_objects = {{ValueKind::Ptr64, bytes, 8, FramePurpose::Alloca}};
    f.blocks = {{0,
                 "entry",
                 {{Opcode::FRAME_ADDR, {O::preg(gpr(15), RegRole::Def), O::frame(0)}},
                  {Opcode::BNEZ, {O::preg(gpr(4)), O::block(1)}},
                  {Opcode::B, {O::block(2)}}},
                 {},
                 {}},
                {1, "yes", {{Opcode::RET, {}}}, {}, {}},
                {2, "no", {{Opcode::RET, {}}}, {}, {}}};
    for (auto id : {1u, 2u}) {
        auto &instructions = f.block(id).instructions;
        instructions.insert(
            instructions.begin(),
            {{Opcode::LOAD_CONSTANT, {O::preg(gpr(23), RegRole::Def), O::immediate(id)}},
             {Opcode::MOVGR2FR_W, {O::preg(fpr(24), RegRole::Def), O::preg(gpr(23))}}});
    }
    rebuild_cfg(f);
    return f;
}
static void require_saved_memory(const MachineInstr &i, Opcode opcode, PReg reg, int64_t offset) {
    require(i.opcode == opcode && i.operands[0].reg == reg &&
                i.operands[0].role == (descriptor(opcode).may_load ? RegRole::Def : RegRole::Use) &&
                i.operands[1].reg == gpr(22) && i.operands[2].imm == offset,
            "saved register has wrong opcode, register, role or frame address");
}
static MachineFunction frame_saved_registers() {
    auto f = saved_frame_fixture();
    verify(MachineModule{{f}, {}}, VerifyStage::Allocated);
    lower_frame(f);
    require(f.frame_objects.size() == 5,
            "callee-saved r23/f24 require two extra saved-register slots");
    for (unsigned n = 1; n <= 2; ++n) {
        const auto &slot = f.frame_objects.at(n);
        require(slot.purpose == FramePurpose::SavedRegister && slot.size == 8 &&
                    slot.alignment == 8 && slot.offset % 8 == 0,
                "callee-saved slots must preserve the full ABI width");
    }
    require(f.frame_objects[0].offset == -40 && f.frame_objects[1].offset == -48 &&
                f.frame_objects[2].offset == -56 && f.frame_size == 64 &&
                f.frame_objects[3].offset == -8 && f.frame_objects[4].offset == -16,
            "save objects must participate in layout without changing ra/fp convention");
    const auto &entry = f.blocks.front().instructions;
    require(entry.at(3).opcode == Opcode::ADDI_D && entry.at(3).operands[0].reg == gpr(22),
            "establish fp before saving extra registers");
    require_saved_memory(entry.at(4), Opcode::ST_D, gpr(23), -48);
    require_saved_memory(entry.at(5), Opcode::FST_D, fpr(24), -56);
    require(entry.at(6).opcode == Opcode::ADDI_D && entry.at(6).operands[0].reg == gpr(15),
            "save before executing original body");
    const auto &exit = f.blocks.back();
    require(exit.instructions.size() == 7, "exactly one unified save/restore epilogue");
    require_saved_memory(exit.instructions.at(0), Opcode::LD_D, gpr(23), -48);
    require_saved_memory(exit.instructions.at(1), Opcode::FLD_D, fpr(24), -56);
    require_saved_memory(exit.instructions.at(2), Opcode::LD_D, gpr(1), -8);
    require_saved_memory(exit.instructions.at(3), Opcode::LD_D, gpr(20), -16);
    require(exit.instructions.at(4).opcode == Opcode::OR &&
                exit.instructions.at(4).operands[0].reg == gpr(3) &&
                exit.instructions.at(5).opcode == Opcode::OR &&
                exit.instructions.at(5).operands[0].reg == gpr(22) &&
                exit.instructions.at(6).opcode == Opcode::RET,
            "restore extras before original ra/sp/fp sequence");
    unsigned returns = 0;
    for (const auto &block : f.blocks)
        for (const auto &i : block.instructions)
            returns += i.opcode == Opcode::RET;
    require(returns == 1, "all original returns must enter unified epilogue");
    for (auto id : {1u, 2u}) {
        const auto &branch = f.block(id).instructions.back();
        require(branch.opcode == Opcode::B && branch.operands[0].id == exit.id &&
                    f.block(id).successors == std::vector<BlockId>{exit.id},
                "both return paths must branch to unified epilogue");
    }
    verify(MachineModule{{f}, {}}, VerifyStage::Final);
    const auto assembly = print_assembly(MachineModule{{f}, {}});
    require(assembly.find("fst.d $f24, $r22, -56") != std::string::npos &&
                assembly.find("fld.d $f24, $r22, -56") != std::string::npos,
            "printer must emit full-width floating save/restore mnemonics");
    return f;
}
static void frame_empty_subset_and_duplicates() {
    for (const auto &registers : std::vector<std::vector<PReg>>{
             {}, {gpr(23)}, {fpr(24)}, {fpr(25), gpr(24), fpr(24), gpr(24), fpr(25)}}) {
        MachineFunction f;
        f.name = "frame_subset";
        f.used_callee_saved = registers;
        f.frame_objects = {{ValueKind::F32, 4, 4, FramePurpose::Spill}};
        f.blocks = {{0, "entry", {{Opcode::RET, {}}}, {}, {}}};
        auto unique = registers;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        lower_frame(f);
        require(f.frame_objects.size() == 3 + unique.size(),
                "save only the sorted unique used subset, including empty set");
        require(f.frame_objects[0].size == 4 && f.frame_objects[0].alignment == 4 &&
                    f.frame_objects[0].purpose == FramePurpose::Spill,
                "ordinary F32 spills must stay four bytes");
        for (unsigned n = 0; n < unique.size(); ++n) {
            const auto reg = unique[n];
            const auto &object = f.frame_objects.at(1 + n);
            require(object.size == 8 && object.alignment == 8 &&
                        object.purpose == FramePurpose::SavedRegister,
                    "subset save width/alignment must use full ABI width");
            require_saved_memory(f.blocks.front().instructions.at(4 + n),
                                 reg.cls == RegClass::FPR ? Opcode::FST_D : Opcode::ST_D, reg,
                                 object.offset);
            require_saved_memory(f.blocks.back().instructions.at(n),
                                 reg.cls == RegClass::FPR ? Opcode::FLD_D : Opcode::LD_D, reg,
                                 object.offset);
        }
        require(f.blocks.front().instructions.size() == 5 + unique.size() &&
                    f.blocks.back().instructions.size() == 5 + unique.size(),
                "unused callee-saved registers must add no extra instructions");
        verify(MachineModule{{f}, {}}, VerifyStage::Final);
    }
}
static void frame_wide_operands() {
    using O = MachineOperand;
    for (auto opcode : {Opcode::FLD_D, Opcode::FST_D}) {
        const auto role = opcode == Opcode::FLD_D ? RegRole::Def : RegRole::Use;
        const auto &d = descriptor(opcode);
        require(d.may_load == (role == RegRole::Def) && d.may_store == (role == RegRole::Use) &&
                    d.operands[0].form == OperandForm::FPR && d.operands[0].role == role &&
                    d.operands[1].form == OperandForm::Address &&
                    d.operands[2].form == OperandForm::Signed12 &&
                    d.mnemonic == (role == RegRole::Def ? "fld.d" : "fst.d"),
                "wide op descriptors must have FPR roles and existing address constraints");
        MachineFunction f;
        f.name = "wide_operand";
        f.frame_objects = {{ValueKind::F32, 8, 8, FramePurpose::SavedRegister}};
        f.blocks = {
            {0,
             "entry",
             {{opcode, {O::preg(fpr(24), role), O::frame(0), O::immediate(0)}}, {Opcode::RET, {}}},
             {},
             {}}};
        verify(MachineModule{{f}, {}}, VerifyStage::Allocated);
        auto bad = f;
        bad.vreg_types = {ValueKind::F32};
        bad.blocks[0].instructions[0].operands[0] = O::vreg(0, role);
        rejected([&] { verify(MachineModule{{bad}, {}}, VerifyStage::Selected); },
                 {f.name, "physical FPR"});
        for (unsigned mutation = 0; mutation < 4; ++mutation) {
            bad = f;
            auto &operands = bad.blocks[0].instructions[0].operands;
            if (mutation == 0)
                operands[0] = O::preg(gpr(23), role);
            else if (mutation == 1)
                operands[0].role = role == RegRole::Def ? RegRole::Use : RegRole::Def;
            else if (mutation == 2)
                operands[1] = O::preg(fpr(24));
            else
                operands[2] = O::immediate(2048);
            rejected([&] { verify(MachineModule{{bad}, {}}, VerifyStage::Allocated); }, {f.name});
        }
    }
}
static MachineFunction frame_large_saved_offsets() {
    auto f = saved_frame_fixture(5120);
    lower_frame(f);
    require(f.frame_size == 5152 && f.frame_objects[0].offset == -5136 &&
                f.frame_objects[1].offset == -5144 && f.frame_objects[2].offset == -5152,
            "large save slots must follow large alloca in the same frame");
    for (const auto &block : f.blocks)
        for (const auto &i : block.instructions)
            for (const auto &operand : i.operands)
                if (operand.is_def())
                    require(operand.reg == gpr(1) || operand.reg == gpr(3) ||
                                operand.reg == gpr(22) || operand.reg == gpr(20) ||
                                operand.reg == gpr(15) || operand.reg == gpr(23) ||
                                operand.reg == fpr(24),
                            "large frame expansion must not clobber an allocation-pool scratch");
    for (const auto *block : {&f.blocks.front(), &f.blocks.back()}) {
        unsigned extras = 0;
        std::map<PReg, int64_t> values{{gpr(3), 0}, {gpr(22), 0}, {gpr(0), 0}};
        for (size_t n = 0; n < block->instructions.size(); ++n) {
            const auto &i = block->instructions[n];
            const auto &o = i.operands;
            if (i.opcode == Opcode::ADDI_D)
                values[o[0].reg] = values.at(o[1].reg) + o[2].imm;
            else if (i.opcode == Opcode::LU12I_W)
                values[o[0].reg] = o[1].imm * 4096;
            else if (i.opcode == Opcode::ORI)
                values[o[0].reg] = values.at(o[1].reg) | o[2].imm;
            else if (i.opcode == Opcode::ADD_D || i.opcode == Opcode::OR)
                values[o[0].reg] = i.opcode == Opcode::ADD_D
                                       ? values.at(o[1].reg) + values.at(o[2].reg)
                                       : values.at(o[1].reg) | values.at(o[2].reg);
            if ((descriptor(i.opcode).may_load || descriptor(i.opcode).may_store) &&
                (o[0].reg == gpr(23) || o[0].reg == fpr(24))) {
                require(n > 0 && block->instructions[n - 1].opcode == Opcode::ADD_D &&
                            block->instructions[n - 1].operands[0].reg == gpr(20) &&
                            block->instructions[n - 1].operands[1].reg == gpr(22) &&
                            block->instructions[n - 1].operands[2].reg == gpr(20) &&
                            o[1].reg == gpr(20) && o[2].imm == 0,
                        "large save/restore addresses must expand only through reserved r20");
                const auto frame = o[0].reg == gpr(23) ? 1u : 2u;
                require(values.at(o[1].reg) + o[2].imm == f.frame_objects[frame].offset &&
                            i.opcode == (block == &f.blocks.front()
                                             ? (frame == 1 ? Opcode::ST_D : Opcode::FST_D)
                                             : (frame == 1 ? Opcode::LD_D : Opcode::FLD_D)),
                        "expanded save/restore must access the exact full-width slot");
                ++extras;
            }
        }
        require(extras == 2, "both large-offset save slots must be accessed");
    }
    verify(MachineModule{{f}, {}}, VerifyStage::Final);
    return f;
}

static void allocation_api_and_report() {
    using O = MachineOperand;
    MachineFunction original;
    original.name = "report";
    original.return_type = ValueKind::I32;
    original.vreg_types = {ValueKind::I32, ValueKind::I32, ValueKind::I32};
    original.blocks = {{7,
                        "entry",
                        {{Opcode::LOAD_CONSTANT, {O::vreg(0, RegRole::Def), O::immediate(5)}},
                         {Opcode::ADD_W, {O::vreg(1, RegRole::Def), O::vreg(0), O::vreg(0)}},
                         {Opcode::B, {O::block(2)}}},
                        {},
                        {}},
                       {2,
                        "exit",
                        {{Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(1)}},
                         {Opcode::RET, {O::preg(gpr(4))}}},
                        {},
                        {}}};
    rebuild_cfg(original);
    verify(MachineModule{{original}, {}}, VerifyStage::PreRA);
    const auto analysis = analyze_liveness(original);
    auto manual = original, convenience = original;
    const auto registers = default_registers();
    auto expected = linear_scan(manual, analysis, registers);
    verify_allocation(manual, analysis, registers, expected);
    rewrite_allocation(manual, expected);
    const auto actual = allocate_linear(convenience);
    verify(MachineModule{{convenience}, {}}, VerifyStage::Allocated);
    require(dump(MachineModule{{manual}, {}}) == dump(MachineModule{{convenience}, {}}) &&
                dump_allocation(manual, analysis, expected) ==
                    dump_allocation(convenience, analysis, actual),
            "allocate_linear must compose analyze/scan/verify/rewrite");
    require(actual.stats.retained == 2 && actual.stats.spilled == 0 && !actual.locations[2],
            "convenience allocation must count only appeared VRegs");
    auto saved = original;
    const RegisterSet saved_registers{{gpr(24), gpr(23)}, {}};
    auto saved_result = linear_scan(saved, analysis, saved_registers);
    verify_allocation(saved, analysis, saved_registers, saved_result);
    rewrite_allocation(saved, saved_result);
    lower_frame(saved);
    const auto saved_report = dump_allocation(saved, analysis, saved_result);
    require(saved_report.find("callee_saved={$r23, $r24}") != std::string::npos &&
                saved_report.find("reloads=0 stores=0 callee_saved=2 frame_size=" +
                                  std::to_string(saved.frame_size)) != std::string::npos,
            "report must sort the saved set and exclude ABI memory traffic from spill counters");
    auto spilled = original;
    RegisterSet empty;
    auto result = linear_scan(spilled, analysis, empty);
    verify_allocation(spilled, analysis, empty, result);
    rewrite_allocation(spilled, result);
    const auto allocated = dump(MachineModule{{spilled}, {}});
    lower_frame(spilled);
    verify(MachineModule{{spilled}, {}}, VerifyStage::Final);
    const auto report = dump_allocation(spilled, analysis, result);
    require(report == dump_allocation(spilled, analysis, result), "allocation report must repeat");
    const auto exit = report.find("bb2 IN={v1, $r1} OUT={}");
    const auto entry = report.find("bb7 IN={$r1} OUT={v1, $r1}");
    require(exit != std::string::npos && entry != std::string::npos && exit < entry,
            "report must sort BlockId while retaining original IN/OUT");
    require(report.find("v0 [1,3) -> slot0") != std::string::npos &&
                report.find("v1 [2,7) -> slot1") != std::string::npos &&
                report.find("v2 [") == std::string::npos,
            "report must use original intervals after instruction/frame mutation");
    require(report.find("slot0 v0 I32 size=4 alignment=4 frame=fi0") != std::string::npos &&
                report.find("slot1 v1 I32 size=4 alignment=4 frame=fi1") != std::string::npos,
            "report must sort and describe spill slots");
    require(report.find("stats retained=2 spilled=2 reloads=2 stores=2 callee_saved=0 frame_size=" +
                        std::to_string(spilled.frame_size)) != std::string::npos,
            "report statistics must exclude ABI saves and include final frame size");
    require(report.find("$r4 [6,8)") != std::string::npos && report.find("0x") == std::string::npos,
            "report must retain original fixed ranges and exclude addresses");
    require(allocated != dump(MachineModule{{spilled}, {}}), "frame lowering must follow snapshot");
}
static void allocation_pipeline() {
    Module module;
    IRBuilder builder(nullptr, &module);
    auto *i32 = module.get_int32_type();
    for (auto name : {"z_first", "a_second"}) {
        auto *function = Function::create(FunctionType::get(i32, {}), name, &module);
        builder.set_insert_point(BasicBlock::create(&module, "entry", function));
        builder.create_ret(ConstantInt::get(9, &module));
    }
    // Historical aggregate initialization still means selection + metrics.
    const Options old_options{SelectionMode::Greedy, false};
    const auto stack = generate(module, old_options);
    auto explicit_stack = old_options;
    explicit_stack.allocation = AllocationMode::Stack;
    require(stack.assembly == generate(module, explicit_stack).assembly && stack.regalloc.empty(),
            "default allocation and old Options aggregate must remain stack");
    auto linear = old_options;
    linear.allocation = AllocationMode::Linear;
    const auto result = generate(module, linear), repeated = generate(module, linear);
    require(result.selected == repeated.selected && result.prera == repeated.prera &&
                result.allocated == repeated.allocated && result.final == repeated.final &&
                result.regalloc == repeated.regalloc && result.assembly == repeated.assembly,
            "all linear pipeline artifacts must be reproducible");
    require(result.regalloc.find("function @z_first") < result.regalloc.find("function @a_second"),
            "report must preserve module function order");
    for (const auto *snapshot : {&stack.allocated, &result.allocated})
        require(snapshot->find("frame=0") != std::string::npos &&
                    snapshot->find("def:%v") == std::string::npos,
                "allocated snapshot must be after allocation and before frame lowering");
    require(result.regalloc.find("callee_saved=0 frame_size=16") != std::string::npos,
            "pipeline report must include final frame layout");
}

static MachineFunction abi_fixture() {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "clobber_saved";
    f.frame_objects = {{ValueKind::I32, 4, 4, FramePurpose::Alloca}};
    f.blocks = {{0, "entry", {}, {}, {}}};
    auto &is = f.block(0).instructions;
    std::vector<VReg> ints, floats;
    for (unsigned n = 0; n < 9; ++n) {
        auto v = f.new_vreg(ValueKind::I32);
        ints.push_back(v);
        is.push_back({Opcode::LOAD_CONSTANT, {O::vreg(v, RegRole::Def), O::immediate(0x3f800000)}});
    }
    for (unsigned n = 0; n < 8; ++n) {
        auto v = f.new_vreg(ValueKind::F32);
        floats.push_back(v);
        is.push_back({Opcode::MOVGR2FR_W, {O::vreg(v, RegRole::Def), O::vreg(ints.front())}});
    }
    for (auto v : ints)
        is.push_back({Opcode::ST_W, {O::vreg(v), O::frame(0), O::immediate(0)}});
    for (auto v : floats)
        is.push_back({Opcode::FST_S, {O::vreg(v), O::frame(0), O::immediate(0)}});
    is.push_back({Opcode::RET, {}});
    return f;
}
static RegisterSet saved_pool() {
    RegisterSet regs;
    for (unsigned n = 23; n <= 31; ++n)
        regs.gprs.push_back(gpr(n));
    for (unsigned n = 24; n <= 31; ++n)
        regs.fprs.push_back(fpr(n));
    return regs;
}
static std::set<PReg> saved_body_defs(const MachineFunction &f) {
    std::set<PReg> written;
    for (const auto &block : f.blocks)
        for (const auto &i : block.instructions)
            for (const auto &o : i.operands)
                if (o.kind == OperandKind::PReg && o.is_def() && is_callee_saved(o.reg))
                    written.insert(o.reg);
    return written;
}
static void abi_coverage() {
    auto f = abi_fixture();
    const auto regs = saved_pool();
    rewrite_checked(f, regs);
    std::set<PReg> expected(regs.gprs.begin(), regs.gprs.end());
    expected.insert(regs.fprs.begin(), regs.fprs.end());
    require(saved_body_defs(f) == expected,
            "ABI allocated body must actually define all r23-r31/f24-f31 before frame lowering");
}

static void write_fixture(const std::string &path, const std::string &text) {
    std::ofstream out(path);
    require(bool(out), "cannot open target fixture");
    out << text;
    require(bool(out), "cannot write target fixture");
}
static void emit_abi(const std::string &directory) {
    std::filesystem::create_directories(directory);
    auto f = abi_fixture();
    const auto regs = saved_pool();
    rewrite_checked(f, regs);
    const auto defs = saved_body_defs(f);
    std::set<PReg> expected(regs.gprs.begin(), regs.gprs.end());
    expected.insert(regs.fprs.begin(), regs.fprs.end());
    require(defs == expected, "ABI manifest requires actual allocated body definitions");
    std::ostringstream manifest;
    manifest << "body_defs";
    for (auto reg : defs)
        manifest << " " << register_name(reg);
    manifest << "\n";
    lower_frame(f);
    verify(MachineModule{{f}, {}}, VerifyStage::Final);
    write_fixture(directory + "/manifest.txt", manifest.str());
    write_fixture(directory + "/clobber_saved.s", print_assembly(MachineModule{{f}, {}}));
    write_fixture(directory + "/main.c",
                  "extern int check_saved(void); int main(void) { return check_saved(); }\n");
    write_fixture(directory + "/bad_saved.s", ".text\n.globl clobber_saved\nclobber_saved:\n"
                                              "    movgr2fr.d $fs0, $zero\n    jr $ra\n");
    write_fixture(directory + "/truncated_saved.s",
                  ".text\n.globl clobber_saved\nclobber_saved:\n"
                  "    addi.d $sp, $sp, -16\n    fst.s $fs0, $sp, 0\n"
                  "    movgr2fr.d $fs0, $zero\n    fld.s $fs0, $sp, 0\n"
                  "    addi.d $sp, $sp, 16\n    jr $ra\n");
}
// Already-lowered MIR keeps multiple values in each bank live across the tested operation.
static MachineFunction pressure_fixture(const std::string &shape) {
    using O = MachineOperand;
    MachineFunction f;
    f.name = "pressure_" + shape;
    f.return_type = ValueKind::I32;
    f.blocks = {{0, "entry", {}, {}, {}}};
    std::vector<VReg> gs, fs;
    const int integers[] = {3, 5, 7, 11};
    const int bits[] = {0x3f800000, 0x40000000, 0x40400000, 0x40800000};
    auto &entry = f.block(0).instructions;
    for (auto n : integers) {
        auto v = f.new_vreg(ValueKind::I32);
        gs.push_back(v);
        entry.push_back({Opcode::LOAD_CONSTANT, {O::vreg(v, RegRole::Def), O::immediate(n)}});
    }
    for (auto n : bits) {
        auto bit = f.new_vreg(ValueKind::I32), v = f.new_vreg(ValueKind::F32);
        fs.push_back(v);
        entry.push_back({Opcode::LOAD_CONSTANT, {O::vreg(bit, RegRole::Def), O::immediate(n)}});
        entry.push_back({Opcode::MOVGR2FR_W, {O::vreg(v, RegRole::Def), O::vreg(bit)}});
    }
    if (shape == "call")
        entry.push_back({Opcode::CALL, {O::sym("pressure_barrier")}});
    if (shape == "loop" || shape == "swap") {
        auto count = f.new_vreg(ValueKind::I32);
        entry.push_back({Opcode::LOAD_CONSTANT, {O::vreg(count, RegRole::Def), O::immediate(3)}});
        entry.push_back({Opcode::B, {O::block(1)}});
        f.blocks.push_back({1, "loop", {}, {}, {}});
        auto &loop = f.block(1).instructions;
        if (shape == "swap") {
            for (auto *values : {&gs, &fs}) {
                auto temp = f.new_vreg(f.vreg_types[values->front()]);
                loop.push_back(
                    {Opcode::COPY, {O::vreg(temp, RegRole::Def), O::vreg((*values)[0])}});
                loop.push_back(
                    {Opcode::COPY, {O::vreg((*values)[0], RegRole::Def), O::vreg((*values)[1])}});
                loop.push_back(
                    {Opcode::COPY, {O::vreg((*values)[1], RegRole::Def), O::vreg(temp)}});
            }
        } else {
            loop.push_back(
                {Opcode::ADD_W, {O::vreg(gs[0], RegRole::Def), O::vreg(gs[0]), O::vreg(gs[1])}});
            loop.push_back(
                {Opcode::FADD_S, {O::vreg(fs[0], RegRole::Def), O::vreg(fs[0]), O::vreg(fs[1])}});
        }
        loop.push_back(
            {Opcode::ADDI_W, {O::vreg(count, RegRole::Def), O::vreg(count), O::immediate(-1)}});
        loop.push_back({Opcode::BNEZ, {O::vreg(count), O::block(1)}});
        loop.push_back({Opcode::B, {O::block(2)}});
        f.blocks.push_back({2, "exit", {}, {}, {}});
    }
    auto &exit = f.blocks.back().instructions;
    // Weighted first elements make swapped values observable rather than relying on a symmetric
    // sum.
    auto ten = f.new_vreg(ValueKind::I32), gi = f.new_vreg(ValueKind::I32);
    auto ft = f.new_vreg(ValueKind::F32), ff = f.new_vreg(ValueKind::F32);
    exit.push_back({Opcode::LOAD_CONSTANT, {O::vreg(ten, RegRole::Def), O::immediate(10)}});
    exit.push_back({Opcode::MUL_W, {O::vreg(gi, RegRole::Def), O::vreg(gs[0]), O::vreg(ten)}});
    exit.push_back({Opcode::MOVGR2FR_W, {O::vreg(ft, RegRole::Def), O::vreg(ten)}});
    exit.push_back({Opcode::FFINT_S_W, {O::vreg(ft, RegRole::Def), O::vreg(ft)}});
    exit.push_back({Opcode::FMUL_S, {O::vreg(ff, RegRole::Def), O::vreg(fs[0]), O::vreg(ft)}});
    for (unsigned n = 1; n < 4; ++n) {
        auto nextg = f.new_vreg(ValueKind::I32), nextf = f.new_vreg(ValueKind::F32);
        exit.push_back(
            {Opcode::ADD_W, {O::vreg(nextg, RegRole::Def), O::vreg(gi), O::vreg(gs[n])}});
        exit.push_back(
            {Opcode::FADD_S, {O::vreg(nextf, RegRole::Def), O::vreg(ff), O::vreg(fs[n])}});
        gi = nextg;
        ff = nextf;
    }
    auto converted = f.new_vreg(ValueKind::F32), fi = f.new_vreg(ValueKind::I32);
    auto sum = f.new_vreg(ValueKind::I32);
    exit.push_back({Opcode::FTINTRZ_W_S, {O::vreg(converted, RegRole::Def), O::vreg(ff)}});
    exit.push_back({Opcode::MOVFR2GR_S, {O::vreg(fi, RegRole::Def), O::vreg(converted)}});
    exit.push_back({Opcode::ADD_W, {O::vreg(sum, RegRole::Def), O::vreg(gi), O::vreg(fi)}});
    exit.push_back({Opcode::COPY, {O::preg(gpr(4), RegRole::Def), O::vreg(sum)}});
    exit.push_back({Opcode::RET, {O::preg(gpr(4))}});
    rebuild_cfg(f);
    verify(MachineModule{{f}, {}}, VerifyStage::PreRA);
    return f;
}
static void emit_pressure(const std::string &directory) {
    std::filesystem::create_directories(directory);
    std::ostringstream manifest, main;
    main << "void pressure_barrier(void) { __asm__ volatile(\"addi.w $a0, $zero, 99\\n"
            "addi.w $t3, $zero, 99\\naddi.w $t4, $zero, 99\\n"
            "movgr2fr.w $fa0, $a0\\nmovgr2fr.w $ft3, $a0\\nmovgr2fr.w $ft4, $a0\""
            " ::: \"a0\", \"t3\", \"t4\", \"f0\", \"f11\", \"f12\"); }\n";
    main << "extern int pressure_straight(void), pressure_loop(void), pressure_swap(void), "
            "pressure_call(void);\n"
            "int main(void) {\n";
    for (auto shape : {"straight", "loop", "swap", "call"}) {
        int expected = std::string(shape) == "loop" ? 282 : std::string(shape) == "swap" ? 99 : 72;
        main << "if (pressure_" << shape << "() != " << expected << ") return 1;\n";
    }
    main << "return 0; }\n";
    write_fixture(directory + "/main.c", main.str());
    for (unsigned pool = 0; pool <= 2; ++pool) {
        RegisterSet regs;
        for (unsigned n = 0; n < pool; ++n) {
            regs.gprs.push_back(gpr(15 + n));
            regs.fprs.push_back(fpr(11 + n));
        }
        for (auto mode : {"stack", "linear"}) {
            MachineModule module;
            for (auto shape : {"straight", "loop", "swap", "call"}) {
                auto f = pressure_fixture(shape);
                unsigned spilled = 0;
                if (std::string(mode) == "linear")
                    spilled = rewrite_checked(f, regs).stats.spilled;
                else {
                    allocate_stack(f);
                    verify(MachineModule{{f}, {}}, VerifyStage::Allocated);
                    spilled = std::count_if(
                        f.frame_objects.begin(), f.frame_objects.end(),
                        [](const FrameObject &o) { return o.purpose == FramePurpose::Spill; });
                }
                require(spilled > 0, "target pressure fixture must actually spill");
                manifest << mode << " " << pool << " " << shape << " " << spilled << "\n";
                lower_frame(f);
                verify(MachineModule{{f}, {}}, VerifyStage::Final);
                module.functions.push_back(std::move(f));
            }
            write_fixture(directory + "/pressure." + mode + "." + std::to_string(pool) + ".s",
                          print_assembly(module));
        }
    }
    write_fixture(directory + "/manifest.txt", manifest.str());
}

int main(int argc, char **argv) {
    try {
        std::string group = argc > 1 ? argv[1] : "all";
        if (group == "emit-abi" || group == "emit-pressure") {
            require(argc == 3, "target generator requires directory");
            if (group == "emit-abi")
                emit_abi(argv[2]);
            else
                emit_pressure(argv[2]);
            return 0;
        }
        require(group == "all" || group == "liveness" || group == "intervals" || group == "fixed" ||
                    group == "scan" || group == "verify" || group == "rewrite" ||
                    group == "frame" || group == "pipeline" || group == "target",
                "unknown register allocation group");
        if (group == "all" || group == "liveness") {
            liveness_chain();
            liveness_diamond();
            liveness_loop();
            liveness_physical();
            liveness_nested_loop_multisweep();
        }
        if (group == "all" || group == "intervals") {
            liveness_chain(true);
            require(!overlaps({1, 6}, {6, 9}), "touching half-open ranges");
            require(overlaps({1, 6}, {5, 9}), "overlapping ranges");
            require(overlaps({5, 9}, {1, 6}), "overlap is symmetric");
            intervals_same_slot();
            intervals_order_and_multiple_defs();
            intervals_empty_passthrough();
            liveness_nested_loop_multisweep();
        }
        if (group == "all" || group == "fixed") {
            fixed_abi();
            fixed_kills_and_usedef();
            fixed_call_result();
            target_registers();
        }
        if (group == "all" || group == "scan") {
            scan_evict_longest();
            scan_boundaries_and_equal_end();
            scan_order_and_victim_ties();
            scan_empty_and_spill_metadata();
            scan_classes_and_callee_saved();
            scan_fixed_constraints();
        }
        if (group == "all" || group == "verify") {
            verify_mutations();
            verify_spill_widths();
            verify_call_and_saved();
        }
        if (group == "all" || group == "rewrite") {
            rewrite_repeated_and_usedef();
            rewrite_sources_and_widths();
            rewrite_copies();
            rewrite_branch_and_call();
            rewrite_phi_swaps();
            rewrite_scratch_conflicts();
            allocated_verifier();
        }
        if (group == "all" || group == "frame") {
            frame_wide_operands();
            auto small = frame_saved_registers();
            frame_empty_subset_and_duplicates();
            auto large = frame_large_saved_offsets();
            if (group == "frame" && argc > 2) {
                std::ofstream output(argv[2]);
                require(bool(output), "cannot open frame assembly fixture output");
                output << print_assembly(MachineModule{{small, large}, {}});
                require(bool(output), "cannot write frame assembly fixture output");
            }
        }
        if (group == "all" || group == "pipeline") {
            allocation_api_and_report();
            allocation_pipeline();
        }
        if (group == "all" || group == "target") {
            abi_coverage();
            auto f = pressure_fixture("straight");
            require(rewrite_checked(f, {{gpr(15), gpr(16)}, {fpr(11), fpr(12)}}).stats.spilled > 0,
                    "target pressure fixture must actually spill");
        }
        std::cout << group << " passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
