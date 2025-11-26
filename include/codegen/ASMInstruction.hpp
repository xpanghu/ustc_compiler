#pragma once

#include <cassert>
#include <string>

struct ASMInstruction {
    enum InstType { Instruction, Attribute, Label, Comment } type;
    std::string content;

    explicit ASMInstruction(std::string s, InstType ty = Instruction)
        : type(ty), content(s) {}

    std::string format() const {
        switch (type) {
        case ASMInstruction::Instruction:
        case ASMInstruction::Attribute:
            return "\t" + content + "\n";
        case ASMInstruction::Label:
            return content + ":\n";
        case ASMInstruction::Comment:
            return "# " + content + "\n";
        }
        assert(false && "unreachable");
    }

    // 用于 lldb 调试生成 summary
    std::string safe_print() const;
};
