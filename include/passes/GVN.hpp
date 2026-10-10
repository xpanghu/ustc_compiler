#pragma once

#include "Constant.hpp"
#include "Dominators.hpp"
#include "FuncInfo.hpp"
#include "Function.hpp"
#include "Module.hpp"
#include "PassManager.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace GVNExpression {
    class ConstFolder {
    public:
        explicit ConstFolder(Module *m) : module_(m) {}
        Constant *compute(Instruction *instr, Constant *value1, Constant *value2);
        Constant *compute(Instruction *instr, Constant *value1);
    private:
        Module *module_;
    };

    // Immutable keys refer to finite class IDs, never to recursively expanded SSA trees.
    struct Expression {
        enum Kind { constant, opaque, operation, phi };
        const Kind kind;
        const std::string type;
        const Instruction::OpID opcode;
        const size_t identity; // constant bits, opaque ValueId, or destination BlockId
        const std::vector<std::string> operand_types;
        const std::vector<size_t> operands;
        const std::vector<size_t> edges;

        Expression(Kind kind, std::string type, Instruction::OpID opcode = Instruction::ret,
                   size_t identity = 0, std::vector<std::string> types = {},
                   std::vector<size_t> operands = {}, std::vector<size_t> edges = {});
        auto fields() const { return std::tie(kind, type, opcode, identity, operand_types, operands, edges); }
        bool operator==(const Expression &other) const { return fields() == other.fields(); }
        bool operator<(const Expression &other) const { return fields() < other.fields(); }
        std::string print() const;
    };
    using Expr = std::shared_ptr<const Expression>;
    bool operator==(const Expr &lhs, const Expr &rhs);
    struct ExprLess {
        bool operator()(const Expr &a, const Expr &b) const { return *a < *b; }
    };
}

struct CongruenceClass {
    size_t index_; // display/canonical label, immutable while in a partition container
    Value *leader_{}; // diagnostic hint; replacement still checks each use
    GVNExpression::Expr value_expr_, value_phi_;
    std::set<GVNExpression::Expr, GVNExpression::ExprLess> expressions_;
    std::set<Value *> members_;
    explicit CongruenceClass(size_t index) : index_(index) {}
    bool operator==(const CongruenceClass &other) const;
};

struct CongruenceClassLess {
    bool operator()(const std::shared_ptr<CongruenceClass> &a,
                    const std::shared_ptr<CongruenceClass> &b) const { return a->index_ < b->index_; }
};

class GVN : public TransformPass {
public:
    using partitions = std::set<std::shared_ptr<CongruenceClass>, CongruenceClassLess>;
    static bool isTop(const partitions &p) { return p.size() == 1 && (*p.begin())->index_ == 0; }
    GVN(Module *m, bool dump_json) : TransformPass(m), dump_json_(dump_json) {}
    void run() override;
    void initPerFunction();
    void detectEquivalences();
    partitions join(const partitions &a, const partitions &b, BasicBlock * = nullptr, BasicBlock * = nullptr);
    std::shared_ptr<CongruenceClass> intersect(const std::shared_ptr<CongruenceClass> &a,
                                             const std::shared_ptr<CongruenceClass> &b);
    void transferFunction(Instruction *inst, partitions &p);
    GVNExpression::Expr valueExpr(const partitions &p, Value *value);
    static std::shared_ptr<CongruenceClass> getVN(const partitions &p, GVNExpression::Expr expression);
    void replace_cc_members(const std::map<BasicBlock *, partitions> &pout);
    static partitions clone(const partitions &p);
    static std::shared_ptr<CongruenceClass> createCongruenceClass(size_t index = 0) {
        return std::make_shared<CongruenceClass>(index);
    }

private:
    using Class = std::shared_ptr<CongruenceClass>;
    bool dump_json_;
    Function *func_{};
    size_t next_id_ = 1;
    std::map<Value *, size_t> value_ids_;
    std::map<std::string, size_t> constant_ids_;
    std::map<BasicBlock *, size_t> block_ids_;
    std::map<Instruction *, size_t> positions_;
    std::vector<BasicBlock *> rpo_;
    std::set<BasicBlock *> reachable_;
    std::map<BasicBlock *, partitions> pin_, pout_;
    std::map<Function *, bool> pure_;
    std::unique_ptr<Dominators> dominators_;
    std::unique_ptr<GVNExpression::ConstFolder> folder_;

    size_t valueId(Value *value);
    size_t classId(const partitions &p, Value *value);
    Class findClass(const partitions &p, Value *value) const;
    void normalize(partitions &p);
    void removeValue(partitions &p, Value *value);
    void addValue(partitions &p, Value *value, Value *equal = nullptr, GVNExpression::Expr expression = nullptr);
    partitions projectEdge(BasicBlock *pred, BasicBlock *dest);
    partitions joinIncoming(BasicBlock *bb);
    bool available(Value *value, Instruction *user, unsigned operand) const;
    Value *representative(const Class &cc, Value *old, Instruction *user, unsigned operand);
    void replaceUses();
    void clearFunction();
    std::string dumpFunction() const;
};

bool operator==(const GVN::partitions &a, const GVN::partitions &b);
