#pragma once

#include "BasicBlock.hpp"
#include "Instruction.hpp"
#include "Value.hpp"

std::string print_as_op(Value *v, bool print_ty);
std::string print_instr_op_name(Instruction::OpID);
