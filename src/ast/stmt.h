#pragma once

#include <vector>
#include <memory>
#include <boost/variant.hpp>
#include "expr.h"

namespace delta {

struct VarDecl;
class Stmt;

enum class StmtKind {
    ReturnStmt,
    VariableStmt,
    IncrementStmt,
    DecrementStmt,
    ExprStmt,
    DeferStmt,
    IfStmt,
    SwitchStmt,
    WhileStmt,
    BreakStmt,
    AssignStmt,
    AugAssignStmt,
};

struct ReturnStmt {
    std::vector<std::unique_ptr<Expr>> values;
    SrcLoc srcLoc;
};

struct VariableStmt {
    VarDecl* decl; // FIXME: decl is owned.
};

struct IncrementStmt {
    std::unique_ptr<Expr> operand;
    SrcLoc srcLoc; // Location of '++'.
};

struct DecrementStmt {
    std::unique_ptr<Expr> operand;
    SrcLoc srcLoc; // Location of '--'.
};

/// A statement that consists of the evaluation of a single expression.
struct ExprStmt {
    std::unique_ptr<Expr> expr;
};

struct DeferStmt {
    std::unique_ptr<Expr> expr;
};

struct IfStmt {
    std::unique_ptr<Expr> condition;
    std::vector<Stmt> thenBody;
    std::vector<Stmt> elseBody;
};

struct SwitchCase {
    std::unique_ptr<Expr> value;
    std::vector<Stmt> stmts;
};

struct SwitchStmt {
    std::unique_ptr<Expr> condition;
    std::vector<SwitchCase> cases;
    std::vector<Stmt> defaultStmts;
};

struct WhileStmt {
    std::unique_ptr<Expr> condition;
    std::vector<Stmt> body;
};

struct BreakStmt {
    SrcLoc srcLoc;
};

struct AssignStmt {
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
    SrcLoc srcLoc; // Location of '='.
};

/// An augmented assignment (a.k.a. compound assignment) statement.
struct AugAssignStmt {
    std::unique_ptr<Expr> lhs;
    std::unique_ptr<Expr> rhs;
    BinaryOperator op;
    SrcLoc srcLoc; // Location of operator symbol.
};

class Stmt {
public:
#define DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(KIND) \
    Stmt(KIND&& value) : data(std::move(value)) { } \
    \
    bool is##KIND() const { return getKind() == StmtKind::KIND; } \
    \
    KIND& get##KIND() { \
        assert(is##KIND()); \
        return boost::get<KIND>(data); \
    } \
    const KIND& get##KIND() const { \
        assert(is##KIND()); \
        return boost::get<KIND>(data); \
    }
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(ReturnStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(VariableStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(IncrementStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(DecrementStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(ExprStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(DeferStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(IfStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(SwitchStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(WhileStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(BreakStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(AssignStmt)
    DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR(AugAssignStmt)
#undef DEFINE_STMTKIND_GETTER_AND_CONSTRUCTOR

    Stmt(Stmt&&) = default;
    StmtKind getKind() const { return static_cast<StmtKind>(data.which()); }

private:
    boost::variant<ReturnStmt, VariableStmt, IncrementStmt, DecrementStmt,
        ExprStmt, DeferStmt, IfStmt, SwitchStmt, WhileStmt, BreakStmt,
        AssignStmt, AugAssignStmt> data;
};

}
