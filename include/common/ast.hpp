#pragma once

#include <cstdarg>
extern "C" {
#include "syntax_tree.h"
extern syntax_tree *parse(const char *input);
}
#include "User.hpp"
#include <memory>
#include <string>
#include <vector>

enum CminusType { TYPE_INT, TYPE_FLOAT, TYPE_VOID };

enum RelOp {
    // <=
    OP_LE,
    // <
    OP_LT,
    // >
    OP_GT,
    // >=
    OP_GE,
    // ==
    OP_EQ,
    // !=
    OP_NEQ
};

enum AddOp {
    // +
    OP_PLUS,
    // -
    OP_MINUS
};

enum MulOp {
    // *
    OP_MUL,
    // /
    OP_DIV
};

class AST;

struct ASTNode;
struct ASTProgram;
struct ASTDeclaration;
struct ASTNum;
struct ASTVarDeclaration;
struct ASTFunDeclaration;
struct ASTParam;

struct ASTCompoundStmt;
struct ASTStatement;
struct ASTExpressionStmt;
struct ASTSelectionStmt;
struct ASTIterationStmt;
struct ASTReturnStmt;
struct ASTFactor;
struct ASTExpression;
struct ASTVar;
struct ASTAssignExpression;
struct ASTSimpleExpression;
struct ASTAdditiveExpression;
struct ASTTerm;
struct ASTCall;

class ASTVisitor;

class AST {
  public:
    AST() = delete;
    AST(syntax_tree *);
    AST(AST &&tree) {
        root = tree.root;
        tree.root = nullptr;
    };
    ASTProgram *get_root() { return root.get(); }
    void run_visitor(ASTVisitor &visitor);

  private:
    ASTNode *transform_node_iter(syntax_tree_node *);
    std::shared_ptr<ASTProgram> root = nullptr;
};

struct ASTNode {
    virtual Value* accept(ASTVisitor &) = 0;
    virtual ~ASTNode() = default;
};

struct ASTProgram : ASTNode {
    Value* accept(ASTVisitor &) final;
    ~ASTProgram() override = default;
    std::vector<std::shared_ptr<ASTDeclaration>> declarations;
};

struct ASTDeclaration : ASTNode {
    ~ASTDeclaration() override = default;
    CminusType type;
    std::string id;
};

struct ASTFactor : ASTNode {
    ~ASTFactor() override = default;
};

struct ASTNum : ASTFactor {
    Value* accept(ASTVisitor &) final;
    CminusType type;
    union {
        int i_val;
        float f_val;
    };
};

struct ASTVarDeclaration : ASTDeclaration {
    Value* accept(ASTVisitor &) final;
    std::shared_ptr<ASTNum> num;
};

struct ASTFunDeclaration : ASTDeclaration {
    Value* accept(ASTVisitor &) final;
    std::vector<std::shared_ptr<ASTParam>> params;
    std::shared_ptr<ASTCompoundStmt> compound_stmt;
};

struct ASTParam : ASTNode {
    Value* accept(ASTVisitor &) final;
    CminusType type;
    std::string id;
    // true if it is array param
    bool isarray;
};

struct ASTStatement : ASTNode {
    ~ASTStatement() override = default;
};

struct ASTCompoundStmt : ASTStatement {
    Value* accept(ASTVisitor &) final;
    std::vector<std::shared_ptr<ASTVarDeclaration>> local_declarations;
    std::vector<std::shared_ptr<ASTStatement>> statement_list;
};

struct ASTExpressionStmt : ASTStatement {
    Value* accept(ASTVisitor &) final;
    std::shared_ptr<ASTExpression> expression;
};

struct ASTSelectionStmt : ASTStatement {
    Value* accept(ASTVisitor &) final;
    std::shared_ptr<ASTExpression> expression;
    std::shared_ptr<ASTStatement> if_statement;
    // should be nullptr if no else structure exists
    std::shared_ptr<ASTStatement> else_statement;
};

struct ASTIterationStmt : ASTStatement {
    Value* accept(ASTVisitor &) final;
    std::shared_ptr<ASTExpression> expression;
    std::shared_ptr<ASTStatement> statement;
};

struct ASTReturnStmt : ASTStatement {
    Value* accept(ASTVisitor &) final;
    // should be nullptr if return void
    std::shared_ptr<ASTExpression> expression;
};

struct ASTExpression : ASTFactor {};

struct ASTAssignExpression : ASTExpression {
    Value* accept(ASTVisitor &) final;
    std::shared_ptr<ASTVar> var;
    std::shared_ptr<ASTExpression> expression;
};

struct ASTSimpleExpression : ASTExpression {
    Value* accept(ASTVisitor &) final;
    std::shared_ptr<ASTAdditiveExpression> additive_expression_l;
    std::shared_ptr<ASTAdditiveExpression> additive_expression_r;
    RelOp op;
};

struct ASTVar : ASTFactor {
    Value* accept(ASTVisitor &) final;
    std::string id;
    // nullptr if var is of int type
    std::shared_ptr<ASTExpression> expression;
};

struct ASTAdditiveExpression : ASTNode {
    Value* accept(ASTVisitor &) final;
    std::shared_ptr<ASTAdditiveExpression> additive_expression;
    AddOp op;
    std::shared_ptr<ASTTerm> term;
};

struct ASTTerm : ASTNode {
    Value* accept(ASTVisitor &) final;
    std::shared_ptr<ASTTerm> term;
    MulOp op;
    std::shared_ptr<ASTFactor> factor;
};

struct ASTCall : ASTFactor {
    Value* accept(ASTVisitor &) final;
    std::string id;
    std::vector<std::shared_ptr<ASTExpression>> args;
};

class ASTVisitor {
  public:
    virtual Value* visit(ASTProgram &) = 0;
    virtual Value* visit(ASTNum &) = 0;
    virtual Value* visit(ASTVarDeclaration &) = 0;
    virtual Value* visit(ASTFunDeclaration &) = 0;
    virtual Value* visit(ASTParam &) = 0;
    virtual Value* visit(ASTCompoundStmt &) = 0;
    virtual Value* visit(ASTExpressionStmt &) = 0;
    virtual Value* visit(ASTSelectionStmt &) = 0;
    virtual Value* visit(ASTIterationStmt &) = 0;
    virtual Value* visit(ASTReturnStmt &) = 0;
    virtual Value* visit(ASTAssignExpression &) = 0;
    virtual Value* visit(ASTSimpleExpression &) = 0;
    virtual Value* visit(ASTAdditiveExpression &) = 0;
    virtual Value* visit(ASTVar &) = 0;
    virtual Value* visit(ASTTerm &) = 0;
    virtual Value* visit(ASTCall &) = 0;
    virtual ~ASTVisitor();
};

class ASTPrinter : public ASTVisitor {
  public:
    Value* visit(ASTProgram &) final;
    Value* visit(ASTNum &) final;
    Value* visit(ASTVarDeclaration &) final;
    Value* visit(ASTFunDeclaration &) final;
    Value* visit(ASTParam &) final;
    Value* visit(ASTCompoundStmt &) final;
    Value* visit(ASTExpressionStmt &) final;
    Value* visit(ASTSelectionStmt &) final;
    Value* visit(ASTIterationStmt &) final;
    Value* visit(ASTReturnStmt &) final;
    Value* visit(ASTAssignExpression &) final;
    Value* visit(ASTSimpleExpression &) final;
    Value* visit(ASTAdditiveExpression &) final;
    Value* visit(ASTVar &) final;
    Value* visit(ASTTerm &) final;
    Value* visit(ASTCall &) final;
    void add_depth() { depth += 2; }
    void remove_depth() { depth -= 2; }

  private:
    int depth = 0;
};
