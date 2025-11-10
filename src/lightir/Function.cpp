#include "Function.hpp"
#include "IRprinter.hpp"
#include "Module.hpp"
#include "Instruction.hpp"
#include <cassert>
#include <unordered_set>
#include <queue>

Function::Function(FunctionType *ty, const std::string &name, Module *parent)
    : Value(ty, name), parent_(parent), seq_cnt_(0) {
    // num_args_ = ty->getNumParams();
    parent->add_function(this);
    // build args
    for (unsigned i = 0; i < get_num_of_args(); i++) {
        arguments_.emplace_back(ty->get_param_type(i), "", this, i);
    }
}
Function *Function::create(FunctionType *ty, const std::string &name,
                           Module *parent) {
    return new Function(ty, name, parent);
}

FunctionType *Function::get_function_type() const {
    return static_cast<FunctionType *>(get_type());
}

Type *Function::get_return_type() const {
    return get_function_type()->get_return_type();
}

unsigned Function::get_num_of_args() const {
    return get_function_type()->get_num_of_args();
}

unsigned Function::get_num_basic_blocks() const { return basic_blocks_.size(); }

Module *Function::get_parent() const { return parent_; }

void Function::remove(BasicBlock *bb) {
    basic_blocks_.remove(bb);
    for (auto pre : bb->get_pre_basic_blocks()) {
        pre->remove_succ_basic_block(bb);
    }
    for (auto succ : bb->get_succ_basic_blocks()) {
        succ->remove_pre_basic_block(bb);
    }
}

void Function::add_basic_block(BasicBlock *bb) { basic_blocks_.push_back(bb); }

void Function::set_instr_name() {
    std::map<Value *, int> seq;
    for (auto &arg : this->get_args()) {
        if (seq.find(&arg) == seq.end()) {
            auto seq_num = seq.size() + seq_cnt_;
            if (arg.set_name("arg" + std::to_string(seq_num))) {
                seq.insert({&arg, seq_num});
            }
        }
    }
    for (auto &bb1 : basic_blocks_) {
        auto bb = &bb1;
        if (seq.find(bb) == seq.end()) {
            auto seq_num = seq.size() + seq_cnt_;
            if (bb->set_name("label" + std::to_string(seq_num))) {
                seq.insert({bb, seq_num});
            }
        }
        for (auto &instr : bb->get_instructions()) {
            if (!instr.is_void() && seq.find(&instr) == seq.end()) {
                auto seq_num = seq.size() + seq_cnt_;
                if (instr.set_name("op" + std::to_string(seq_num))) {
                    seq.insert({&instr, seq_num});
                }
            }
        }
    }
    seq_cnt_ += seq.size();
}

std::string Function::print() {
    set_instr_name();
    std::string func_ir;
    if (this->is_declaration()) {
        func_ir += "declare ";
    } else {
        func_ir += "define ";
    }

    func_ir += this->get_return_type()->print();
    func_ir += " ";
    func_ir += print_as_op(this, false);
    func_ir += "(";

    // print arg
    if (this->is_declaration()) {
        for (unsigned i = 0; i < this->get_num_of_args(); i++) {
            if (i)
                func_ir += ", ";
            func_ir += static_cast<FunctionType *>(this->get_type())
                           ->get_param_type(i)
                           ->print();
        }
    } else {
        for (auto &arg : get_args()) {
            if (&arg != &*get_args().begin())
                func_ir += ", ";
            func_ir += arg.print();
        }
    }
    func_ir += ")";

    // print bb
    if (this->is_declaration()) {
        func_ir += "\n";
    } else {
        func_ir += " {";
        func_ir += "\n";
        for (auto &bb1 : this->get_basic_blocks()) {
            auto bb = &bb1;
            func_ir += bb->print();
        }
        func_ir += "}";
    }

    return func_ir;
}

std::string Argument::print() {
    std::string arg_ir;
    arg_ir += this->get_type()->print();
    arg_ir += " %";
    arg_ir += this->get_name();
    return arg_ir;
}


void Function::check_for_block_relation_error()
{
    // 检查函数的基本块表是否包含所有且仅包含 get_parent 是本函数的基本块
    std::unordered_set<BasicBlock*> bbs;
    for (auto& bb : basic_blocks_)
    {
        bbs.emplace(&bb);
    }
    for (auto& bb : basic_blocks_)
    {
        assert((bb.get_parent() == this) && "函数 F 的基本块表中包含基本块 a, 但 a 的 get_parent 方法不返回 F");
        for (auto i: bb.get_succ_basic_blocks())
        {
            assert((bbs.count(i)) && "函数 F 的基本块表中包含基本块 a, a 的后继块表中的某基本块 b 不在 F 的基本块表中");
        }
        for (auto i: bb.get_pre_basic_blocks())
        {
            assert((bbs.count(i)) && "函数 F 的基本块表中包含基本块 a, a 的前驱块表中的某基本块 b 不在 F 的基本块表中");
        }
    }
    // 检查基本块的前驱和后继表不包含重复的基本块
    /*
	for (auto& bb : basic_blocks_)
    {
        bbs.clear();
        for(auto suc: bb.get_succ_basic_blocks()){
            assert((!bbs.count(suc)) && "基本块的后继表中包含重复项目");
            bbs.emplace(suc);
        }
        for(auto suc: bb.get_pre_basic_blocks()){
            assert((!bbs.count(suc)) && "基本块的前驱表中包含重复项目");
            bbs.emplace(suc);
        }
    }
	*/
    // 检查基本块基本信息
    for (auto& bb : basic_blocks_)
    {
        assert((!bb.get_instructions().empty()) && "发现了空基本块");
        auto b = &bb.get_instructions().back();
        assert((b->is_br() || b->is_ret()) && "发现了无 terminator 基本块");
        assert((b->is_br() || bb.get_succ_basic_blocks().empty()) && "某基本块末尾是 ret 指令但是后继块表不是空的, 或者末尾是 br 但后继表为空");
    }
    // 检查基本块前驱后继关系是否是与 branch 指令对应
    for (auto& bb : basic_blocks_)
    {
        if(!bb.get_succ_basic_blocks().empty()){
            std::unordered_set<BasicBlock*> suc_table;
            std::unordered_set<BasicBlock*> br_get;
            for (auto suc : bb.get_succ_basic_blocks())
                suc_table.emplace(suc);
            auto& ops = bb.get_instructions().back().get_operands();
            for (auto i : ops)
            {
                auto bb2 = dynamic_cast<BasicBlock*>(i);
                if(bb2 != nullptr) br_get.emplace(bb2);
            }
            for(auto i : suc_table)
                assert(br_get.count(i) && "基本块 A 的后继块有 B，但 B 并未在 A 的 br 指令中出现");
            for(auto i : br_get)
                assert(suc_table.count(i) && "基本块 A 的后继块没有 B，但 B 在 A 的 br 指令中出现了");
            for(auto i : suc_table) {
                bool ok = false;
                for(auto j : i->get_pre_basic_blocks()) {
                    if(j == &bb){
                        ok = true;
                        break;
                    }
                }
                assert(ok && "基本块 A 的后继块表中有 B，但 B 的前驱表中没有 A");
            }
        }
    }
    // 检查基本块前驱后继关系是否是与 branch 指令对应
    for (auto& bb : basic_blocks_)
    {
        for (auto pre : bb.get_pre_basic_blocks())
        {
            bool ok = false;
            for (auto i : pre->get_succ_basic_blocks())
            {
                if (i == &bb)
                {
                    ok = true;
                    break;
                }
            }
            if (!ok) 
                assert(false && "基本块 A 的后继块表中没有 B，但 B 的前驱表中有 A");
        }
    }
    // 检查指令 parent 设置
    for (auto& bb : basic_blocks_)
    {
        for (auto& inst : bb.get_instructions())
        {
            assert ((inst.get_parent() == &bb) && "基本块 A 指令表包含指令 b, 但是 b 的 get_parent 函数不返回 A");
        }
    }
}
