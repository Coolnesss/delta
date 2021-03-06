#include <vector>
#include <forward_list>
#include <sstream>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/MemoryBuffer.h>
#include "parse.h"
#include "lex.h"
#include "../ast/token.h"
#include "../ast/decl.h"
#include "../ast/module.h"
#include "../sema/typecheck.h"
#include "../support/utility.h"

using namespace delta;

namespace delta {
extern const char* currentFilePath;
}

namespace {

std::vector<Token> tokenBuffer;
size_t currentTokenIndex;
Module* currentModule;

Token currentToken() {
    ASSERT(currentTokenIndex < tokenBuffer.size());
    return tokenBuffer[currentTokenIndex];
}

SourceLocation getCurrentLocation() {
    return currentToken().getLocation();
}

Token lookAhead(int offset) {
    if (int(currentTokenIndex) + offset < 0) return NO_TOKEN;
    int count = int(currentTokenIndex) + offset - int(tokenBuffer.size()) + 1;
    while (count-- > 0) tokenBuffer.emplace_back(lex());
    return tokenBuffer[currentTokenIndex + offset];
}

Token consumeToken() {
    Token token = currentToken();
    if (++currentTokenIndex == tokenBuffer.size())
        tokenBuffer.emplace_back(lex());
    return token;
}

/// Adds quotes around the string representation of the given token unless
/// it's an identifier, numeric literal, string literal, or end-of-file.
std::string quote(TokenKind tokenKind) {
    std::ostringstream stream;
    if (tokenKind < BREAK) {
        stream << tokenKind;
    } else {
        stream << '\'' << tokenKind << '\'';
    }
    return stream.str();
}

[[noreturn]] void unexpectedToken(Token token, llvm::ArrayRef<TokenKind> expected = {},
                                  const char* contextInfo = nullptr) {
    if (expected.size() == 0) {
        error(token.getLocation(), "unexpected ", quote(token),
              contextInfo ? " " : "", contextInfo ? contextInfo : "");
    } else {
        error(token.getLocation(), "expected ", toDisjunctiveList(expected, quote),
              contextInfo ? " " : "", contextInfo ? contextInfo : "",
              ", got ", quote(token));
    }
}

void expect(llvm::ArrayRef<TokenKind> expected, const char* contextInfo) {
    if (!llvm::is_contained(expected, currentToken())) {
        unexpectedToken(currentToken(), expected, contextInfo);
    }
}

Token parse(llvm::ArrayRef<TokenKind> expected, const char* contextInfo = nullptr) {
    expect(expected, contextInfo);
    return consumeToken();
}

static void checkStmtTerminatorConsistency(TokenKind currentTerminator,
                                           llvm::function_ref<SourceLocation()> getLocation) {
    static TokenKind previousTerminator = NO_TOKEN;
    static const char* filePath = nullptr;

    if (filePath != delta::currentFilePath) {
        filePath = delta::currentFilePath;
        previousTerminator = NO_TOKEN;
    }

    if (previousTerminator == NO_TOKEN) {
        previousTerminator = currentTerminator;
    } else if (previousTerminator != currentTerminator) {
        warning(getLocation(), "inconsistent statement terminator, expected ", quote(previousTerminator));
    }
}

void parseStmtTerminator(const char* contextInfo = nullptr) {
    if (getCurrentLocation().line != lookAhead(-1).getLocation().line) {
        checkStmtTerminatorConsistency(NEWLINE, [] {
            // TODO: Use pre-existing buffer instead of reading from file here.
            readLineFromFile(lookAhead(-1).getLocation());
            std::ifstream file(getCurrentLocation().file);
            for (auto line = lookAhead(-1).getLocation().line; --line;) {
                file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            }
            std::string line;
            std::getline(file, line);
            return SourceLocation(getCurrentLocation().file, lookAhead(-1).getLocation().line, line.size() + 1);
        });
        return;
    }

    switch (currentToken()) {
        case RBRACE:
            checkStmtTerminatorConsistency(NEWLINE, getCurrentLocation);
            return;
        case SEMICOLON:
            checkStmtTerminatorConsistency(SEMICOLON, getCurrentLocation);
            consumeToken();
            return;
        default:
            unexpectedToken(currentToken(), { NEWLINE, SEMICOLON }, contextInfo);
    }
}

std::unique_ptr<Expr> parseExpr();
std::unique_ptr<Expr> parsePreOrPostfixExpr();
std::vector<std::unique_ptr<Expr>> parseExprList();
std::vector<std::unique_ptr<Stmt>> parseStmtsUntil(Token end);
std::vector<std::unique_ptr<Stmt>> parseStmtsUntilOneOf(Token end1, Token end2, Token end3);
Type parseType();

/// argument-list ::= '(' ')' | '(' nonempty-argument-list ')'
/// nonempty-argument-list ::= argument | nonempty-argument-list ',' argument
/// argument ::= (id ':')? expr
std::vector<Argument> parseArgumentList() {
    parse(LPAREN);
    std::vector<Argument> args;
    while (currentToken() != RPAREN) {
        std::string name;
        SourceLocation location = SourceLocation::invalid();
        if (lookAhead(1) == COLON) {
            auto result = parse(IDENTIFIER);
            name = std::move(result.getString());
            location = result.getLocation();
            consumeToken();
        }
        auto value = parseExpr();
        if (!location.isValid()) location = value->getLocation();
        args.push_back({ std::move(name), std::move(value), location });
        if (currentToken() != RPAREN) parse(COMMA);
    }
    consumeToken();
    return args;
}

/// var-expr ::= id
std::unique_ptr<VarExpr> parseVarExpr() {
    ASSERT(currentToken() == IDENTIFIER);
    auto id = parse(IDENTIFIER);
    return llvm::make_unique<VarExpr>(id.getString(), id.getLocation());
}

std::unique_ptr<VarExpr> parseThis() {
    ASSERT(currentToken() == THIS);
    auto expr = llvm::make_unique<VarExpr>("this", getCurrentLocation());
    consumeToken();
    return expr;
}

std::string replaceEscapeChars(llvm::StringRef literalContent, SourceLocation literalStartLocation) {
    std::string result;
    result.reserve(literalContent.size());

    for (auto it = literalContent.begin(), end = literalContent.end(); it != end; ++it) {
        if (*it == '\\') {
            ++it;
            ASSERT(it != end);
            switch (*it) {
                case 'a': result += '\a'; break;
                case 'b': result += '\b'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'v': result += '\v'; break;
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                default:
                    auto itColumn = literalStartLocation.column + 1 + (it - literalContent.begin());
                    SourceLocation itLocation(literalStartLocation.file, literalStartLocation.line, itColumn);
                    error(itLocation, "unknown escape character '\\", *it, "'");
            }
            continue;
        }
        result += *it;
    }
    return result;
}

std::unique_ptr<StringLiteralExpr> parseStringLiteral() {
    ASSERT(currentToken() == STRING_LITERAL);
    auto content = replaceEscapeChars(currentToken().getString().drop_back().drop_front(), getCurrentLocation());
    auto expr = llvm::make_unique<StringLiteralExpr>(std::move(content), getCurrentLocation());
    consumeToken();
    return expr;
}

std::unique_ptr<IntLiteralExpr> parseIntLiteral() {
    ASSERT(currentToken() == INT_LITERAL);
    auto expr = llvm::make_unique<IntLiteralExpr>(currentToken().getIntegerValue(),
                                                  getCurrentLocation());
    consumeToken();
    return expr;
}

std::unique_ptr<FloatLiteralExpr> parseFloatLiteral() {
    ASSERT(currentToken() == FLOAT_LITERAL);
    auto expr = llvm::make_unique<FloatLiteralExpr>(currentToken().getFloatingPointValue(),
                                                    getCurrentLocation());
    consumeToken();
    return expr;
}

std::unique_ptr<BoolLiteralExpr> parseBoolLiteral() {
    std::unique_ptr<BoolLiteralExpr> expr;
    switch (currentToken()) {
        case TRUE: expr = llvm::make_unique<BoolLiteralExpr>(true, getCurrentLocation()); break;
        case FALSE: expr = llvm::make_unique<BoolLiteralExpr>(false, getCurrentLocation()); break;
        default: llvm_unreachable("all cases handled");
    }
    consumeToken();
    return expr;
}

std::unique_ptr<NullLiteralExpr> parseNullLiteral() {
    ASSERT(currentToken() == NULL_LITERAL);
    auto expr = llvm::make_unique<NullLiteralExpr>(getCurrentLocation());
    consumeToken();
    return expr;
}

/// array-literal ::= '[' expr-list ']'
std::unique_ptr<ArrayLiteralExpr> parseArrayLiteral() {
    ASSERT(currentToken() == LBRACKET);
    auto location = getCurrentLocation();
    consumeToken();
    auto elements = parseExprList();
    parse(RBRACKET);
    return llvm::make_unique<ArrayLiteralExpr>(std::move(elements), location);
}

/// generic-argument-list ::= '<' generic-arguments '>'
/// generic-arguments ::= type | type ',' generic-arguments
std::vector<Type> parseGenericArgumentList() {
    ASSERT(currentToken() == LT);
    consumeToken();
    std::vector<Type> genericArgs;

    while (true) {
        genericArgs.emplace_back(parseType());
        if (currentToken() == GT) break;
        parse(COMMA);
    }

    consumeToken();
    return genericArgs;
}

/// simple-type ::= id | id generic-argument-list | id '[' int-literal? ']'
Type parseSimpleType(bool isMutable) {
    ASSERT(currentToken() == IDENTIFIER);
    llvm::StringRef id = consumeToken().getString();

    Type type;
    std::vector<Type> genericArgs;

    switch (currentToken()) {
        case LT:
            genericArgs = parseGenericArgumentList();
            LLVM_FALLTHROUGH;
        default:
            return BasicType::get(id, std::move(genericArgs), isMutable);
        case LBRACKET:
            consumeToken();
            type = BasicType::get(id, {}, isMutable);
            break;
    }

    int64_t arraySize;
    if (currentToken() == RBRACKET)
        arraySize = ArrayType::unsized;
    else if (currentToken() == INT_LITERAL)
        arraySize = consumeToken().getIntegerValue();
    else
        error(getCurrentLocation(), "non-literal array bounds not implemented yet");

    parse(RBRACKET);
    return ArrayType::get(type, arraySize);
}

// type ::= simple-type | 'mutable' simple-type | 'mutable' '(' type ')' | type '&' | type '*'
Type parseType() {
    Type type;
    switch (currentToken()) {
        case IDENTIFIER:
            type = parseSimpleType(false);
            break;
        case MUTABLE:
            consumeToken();
            if (currentToken() != LPAREN) {
                type = parseSimpleType(true);
            } else {
                consumeToken();
                type = parseType();
                parse(RPAREN);
            }
            type.setMutable(true);
            break;
        default:
            unexpectedToken(currentToken(), { IDENTIFIER, MUTABLE });
    }
    while (true) {
        switch (currentToken()) {
            case AND: case STAR:
                type = PointerType::get(type, currentToken() == AND);
                consumeToken();
                break;
            default:
                return type;
        }
    }
}

/// cast-expr ::= 'cast' '<' type '>' '(' expr ')'
std::unique_ptr<CastExpr> parseCastExpr() {
    ASSERT(currentToken() == CAST);
    auto location = getCurrentLocation();
    consumeToken();
    parse(LT);
    auto type = parseType();
    parse(GT);
    parse(LPAREN);
    auto expr = parseExpr();
    parse(RPAREN);
    return llvm::make_unique<CastExpr>(type, std::move(expr), location);
}

/// member-expr ::= expr '.' id
std::unique_ptr<MemberExpr> parseMemberExpr(std::unique_ptr<Expr> lhs) {
    auto member = parse(IDENTIFIER);
    return llvm::make_unique<MemberExpr>(std::move(lhs), member.getString(), member.getLocation());
}

/// subscript-expr ::= expr '[' expr ']'
std::unique_ptr<SubscriptExpr> parseSubscript(std::unique_ptr<Expr> operand) {
    ASSERT(currentToken() == LBRACKET);
    auto location = getCurrentLocation();
    consumeToken();
    auto index = parseExpr();
    parse(RBRACKET);
    return llvm::make_unique<SubscriptExpr>(std::move(operand), std::move(index), location);
}

/// unwrap-expr ::= expr '!'
std::unique_ptr<UnwrapExpr> parseUnwrapExpr(std::unique_ptr<Expr> operand) {
    ASSERT(currentToken() == NOT);
    auto location = getCurrentLocation();
    consumeToken();
    return llvm::make_unique<UnwrapExpr>(std::move(operand), location);
}

/// call-expr ::= expr generic-argument-list? '(' arguments ')'
std::unique_ptr<CallExpr> parseCallExpr(std::unique_ptr<Expr> callee) {
    std::vector<Type> genericArgs;
    if (currentToken() == LT) {
        genericArgs = parseGenericArgumentList();
    }
    auto location = getCurrentLocation();
    auto args = parseArgumentList();
    return llvm::make_unique<CallExpr>(std::move(callee), std::move(args),
                                       std::move(genericArgs), location);
}

/// paren-expr ::= '(' expr ')'
std::unique_ptr<Expr> parseParenExpr() {
    ASSERT(currentToken() == LPAREN);
    consumeToken();
    auto expr = parseExpr();
    parse(RPAREN);
    return expr;
}

bool shouldParseGenericArgumentList() {
    // Temporary hack: use spacing to determine whether to parse a generic argument list
    // of a less-than binary expression. Zero spaces on either side of '<' will cause it
    // to be interpreted as a generic argument list, for now.
    return lookAhead(0).getLocation().column + int(lookAhead(0).getString().size()) == lookAhead(1).getLocation().column
           || lookAhead(1).getLocation().column + 1 == lookAhead(2).getLocation().column;
}

/// postfix-expr ::= postfix-expr postfix-op | call-expr | variable-expr | string-literal |
///                  int-literal | float-literal | bool-literal | null-literal |
///                  paren-expr | array-literal | cast-expr | subscript-expr | member-expr
///                  unwrap-expr
std::unique_ptr<Expr> parsePostfixExpr() {
    std::unique_ptr<Expr> expr;
    switch (currentToken()) {
        case IDENTIFIER:
            switch (lookAhead(1)) {
                case LPAREN: expr = parseCallExpr(parseVarExpr()); break;
                case LT:
                    if (shouldParseGenericArgumentList()) {
                        expr = parseCallExpr(parseVarExpr());
                        break;
                    }
                    LLVM_FALLTHROUGH;
                default: expr = parseVarExpr(); break;
            }
            break;
        case STRING_LITERAL: expr = parseStringLiteral(); break;
        case INT_LITERAL: expr = parseIntLiteral(); break;
        case FLOAT_LITERAL: expr = parseFloatLiteral(); break;
        case TRUE: case FALSE: expr = parseBoolLiteral(); break;
        case NULL_LITERAL: expr = parseNullLiteral(); break;
        case THIS: expr = parseThis(); break;
        case LPAREN: expr = parseParenExpr(); break;
        case LBRACKET: expr = parseArrayLiteral(); break;
        case CAST: expr = parseCastExpr(); break;
        default: unexpectedToken(currentToken()); break;
    }
    while (true) {
        switch (currentToken()) {
            case LBRACKET:
                expr = parseSubscript(std::move(expr));
                break;
            case LPAREN:
                expr = parseCallExpr(std::move(expr));
                break;
            case DOT:
                consumeToken();
                expr = parseMemberExpr(std::move(expr));
                break;
            case NOT:
                expr = parseUnwrapExpr(std::move(expr));
                break;
            default:
                return expr;
        }
    }
}

/// prefix-expr ::= prefix-operator (prefix-expr | postfix-expr)
std::unique_ptr<PrefixExpr> parsePrefixExpr() {
    ASSERT(currentToken().isPrefixOperator());
    auto op = currentToken();
    auto location = getCurrentLocation();
    consumeToken();
    return llvm::make_unique<PrefixExpr>(op, parsePreOrPostfixExpr(), location);
}

std::unique_ptr<Expr> parsePreOrPostfixExpr() {
    return currentToken().isPrefixOperator() ? parsePrefixExpr() : parsePostfixExpr();
}

/// binary-expr ::= expr op expr
std::unique_ptr<Expr> parseBinaryExpr(std::unique_ptr<Expr> lhs, int minPrecedence) {
    while (currentToken().isBinaryOperator() && currentToken().getPrecedence() >= minPrecedence) {
        auto backtrackLocation = currentTokenIndex;
        auto op = consumeToken();
        auto expr = parsePreOrPostfixExpr();
        if (currentToken().isAssignmentOperator()) {
            currentTokenIndex = backtrackLocation;
            break;
        }

        while (currentToken().isBinaryOperator() && currentToken().getPrecedence() > op.getPrecedence()) {
            auto token = consumeToken();
            expr = llvm::make_unique<BinaryExpr>(token, std::move(expr), parsePreOrPostfixExpr(),
                                                 token.getLocation());
        }
        lhs = llvm::make_unique<BinaryExpr>(op, std::move(lhs), std::move(expr), op.getLocation());
    }
    return lhs;
}

/// expr ::= prefix-expr | postfix-expr | binary-expr
std::unique_ptr<Expr> parseExpr() {
    return parseBinaryExpr(parsePreOrPostfixExpr(), 0);
}

/// assign-stmt ::= expr '=' expr ('\n' | ';')
std::unique_ptr<AssignStmt> parseAssignStmt(std::unique_ptr<Expr> lhs) {
    auto location = getCurrentLocation();
    parse(ASSIGN);
    auto rhs = parseExpr();
    parseStmtTerminator();
    return llvm::make_unique<AssignStmt>(std::move(lhs), std::move(rhs),
                                         /* isCompoundAssignment */ false, location);
}

/// compound-assign-stmt ::= expr compound-assignment-op expr ('\n' | ';')
std::unique_ptr<AssignStmt> parseCompoundAssignStmt(std::unique_ptr<Expr> lhs = nullptr) {
    if (!lhs) lhs = parseExpr();
    SourceLocation location = getCurrentLocation();
    auto op = BinaryOperator(consumeToken().withoutCompoundEqSuffix());
    auto rhs = parseExpr();
    parseStmtTerminator();

    std::shared_ptr<Expr> sharedLHS = std::move(lhs);
    auto binaryExpr = llvm::make_unique<BinaryExpr>(op, std::shared_ptr<Expr>(sharedLHS),
                                                    std::move(rhs), location);
    return llvm::make_unique<AssignStmt>(std::move(sharedLHS), std::move(binaryExpr),
                                         /* isCompoundAssignment */ true, location);
}

/// expr-list ::= '' | nonempty-expr-list
/// nonempty-expr-list ::= expr | expr ',' nonempty-expr-list
std::vector<std::unique_ptr<Expr>> parseExprList() {
    std::vector<std::unique_ptr<Expr>> exprs;

    // TODO: Handle empty expression list.
    if (currentToken() == SEMICOLON || currentToken() == RBRACE) {
        return exprs;
    }

    while (true) {
        exprs.emplace_back(parseExpr());
        if (currentToken() != COMMA) return exprs;
        consumeToken();
    }
}

/// return-stmt ::= 'return' expr-list ('\n' | ';')
std::unique_ptr<ReturnStmt> parseReturnStmt() {
    ASSERT(currentToken() == RETURN);
    auto location = getCurrentLocation();
    consumeToken();
    auto returnValues = parseExprList();
    parseStmtTerminator();
    return llvm::make_unique<ReturnStmt>(std::move(returnValues), location);
}

/// var-decl ::= mutability-specifier id type-specifier? '=' initializer ('\n' | ';')
/// mutability-specifier ::= 'let' | 'var'
/// type-specifier ::= ':' type
/// initializer ::= expr | 'uninitialized'
std::unique_ptr<VarDecl> parseVarDecl() {
    ASSERT(currentToken().is(LET, VAR));
    bool isMutable = consumeToken() == VAR;
    auto name = parse(IDENTIFIER);

    Type type;
    if (currentToken() == COLON) {
        consumeToken();
        auto typeLocation = getCurrentLocation();
        type = parseType();
        if (type.isMutable()) error(typeLocation, "type specifier cannot specify mutability");
    }
    type.setMutable(isMutable);

    parse(ASSIGN);
    auto initializer = currentToken() != UNINITIALIZED ? parseExpr() : nullptr;
    if (!initializer) consumeToken();
    parseStmtTerminator();
    return llvm::make_unique<VarDecl>(type, name.getString(), std::move(initializer),
                                      *currentModule, name.getLocation());
}

/// var-stmt ::= var-decl
std::unique_ptr<VarStmt> parseVarStmt() {
    return llvm::make_unique<VarStmt>(parseVarDecl());
}

/// call-stmt ::= call-expr ('\n' | ';')
std::unique_ptr<ExprStmt> parseCallStmt(std::unique_ptr<Expr> callExpr) {
    ASSERT(callExpr->isCallExpr());
    auto stmt = llvm::make_unique<ExprStmt>(std::move(callExpr));
    parseStmtTerminator();
    return stmt;
}

/// inc-stmt ::= expr '++' ('\n' | ';')
std::unique_ptr<IncrementStmt> parseIncrementStmt(std::unique_ptr<Expr> operand) {
    auto location = getCurrentLocation();
    parse(INCREMENT);
    parseStmtTerminator();
    return llvm::make_unique<IncrementStmt>(std::move(operand), location);
}

/// dec-stmt ::= expr '--' ('\n' | ';')
std::unique_ptr<DecrementStmt> parseDecrementStmt(std::unique_ptr<Expr> operand) {
    auto location = getCurrentLocation();
    parse(DECREMENT);
    parseStmtTerminator();
    return llvm::make_unique<DecrementStmt>(std::move(operand), location);
}

/// defer-stmt ::= 'defer' call-expr ('\n' | ';')
std::unique_ptr<DeferStmt> parseDeferStmt() {
    ASSERT(currentToken() == DEFER);
    consumeToken();
    // FIXME: Doesn't have to be a variable expression.
    auto stmt = llvm::make_unique<DeferStmt>(parseCallExpr(parseVarExpr()));
    parseStmtTerminator();
    return stmt;
}

/// if-stmt ::= 'if' '(' expr ')' '{' stmt* '}' ('else' else-branch)?
/// else-branch ::= if-stmt | '{' stmt* '}'
std::unique_ptr<IfStmt> parseIfStmt() {
    ASSERT(currentToken() == IF);
    consumeToken();
    parse(LPAREN);
    auto condition = parseExpr();
    parse(RPAREN);
    parse(LBRACE);
    auto thenStmts = parseStmtsUntil(RBRACE);
    parse(RBRACE);
    std::vector<std::unique_ptr<Stmt>> elseStmts;
    if (currentToken() != ELSE)
        return llvm::make_unique<IfStmt>(std::move(condition), std::move(thenStmts),
                                         std::move(elseStmts));
    consumeToken();
    if (currentToken() == LBRACE) {
        consumeToken();
        elseStmts = parseStmtsUntil(RBRACE);
        parse(RBRACE);
        return llvm::make_unique<IfStmt>(std::move(condition), std::move(thenStmts),
                                         std::move(elseStmts));
    }
    if (currentToken() == IF) {
        elseStmts.emplace_back(parseIfStmt());
        return llvm::make_unique<IfStmt>(std::move(condition), std::move(thenStmts),
                                         std::move(elseStmts));
    }
    unexpectedToken(currentToken(), { LBRACE, IF });
}

/// while-stmt ::= 'while' '(' expr ')' '{' stmt* '}'
std::unique_ptr<WhileStmt> parseWhileStmt() {
    ASSERT(currentToken() == WHILE);
    consumeToken();
    parse(LPAREN);
    auto condition = parseExpr();
    parse(RPAREN);
    parse(LBRACE);
    auto body = parseStmtsUntil(RBRACE);
    parse(RBRACE);
    return llvm::make_unique<WhileStmt>(std::move(condition), std::move(body));
}

/// for-stmt ::= 'for' '(' id 'in' expr ')' '{' stmt* '}'
std::unique_ptr<ForStmt> parseForStmt() {
    ASSERT(currentToken() == FOR);
    consumeToken();
    parse(LPAREN);
    auto id = parse(IDENTIFIER);
    parse(IN);
    auto range = parseExpr();
    parse(RPAREN);
    parse(LBRACE);
    auto body = parseStmtsUntil(RBRACE);
    parse(RBRACE);
    return llvm::make_unique<ForStmt>(std::string(id.getString()), std::move(range),
                                      std::move(body), id.getLocation());
}

/// switch-stmt ::= 'switch' '(' expr ')' '{' cases default-case? '}'
/// cases ::= case | case cases
/// case ::= 'case' expr ':' stmt+
/// default-case ::= 'default' ':' stmt+
std::unique_ptr<SwitchStmt> parseSwitchStmt() {
    ASSERT(currentToken() == SWITCH);
    consumeToken();
    parse(LPAREN);
    auto condition = parseExpr();
    parse(RPAREN);
    parse(LBRACE);
    std::vector<SwitchCase> cases;
    std::vector<std::unique_ptr<Stmt>> defaultStmts;
    bool defaultSeen = false;
    while (true) {
        if (currentToken() == CASE) {
            consumeToken();
            auto value = parseExpr();
            parse(COLON);
            auto stmts = parseStmtsUntilOneOf(CASE, DEFAULT, RBRACE);
            cases.push_back({ std::move(value), std::move(stmts) });
        } else if (currentToken() == DEFAULT) {
            if (defaultSeen)
                error(getCurrentLocation(), "switch-statement may only contain one 'default' case");
            consumeToken();
            parse(COLON);
            defaultStmts = parseStmtsUntilOneOf(CASE, DEFAULT, RBRACE);
            defaultSeen = true;
        } else {
            error(getCurrentLocation(), "expected 'case' or 'default'");
        }
        if (currentToken() == RBRACE) break;
    }
    consumeToken();
    return llvm::make_unique<SwitchStmt>(std::move(condition), std::move(cases),
                                         std::move(defaultStmts));
}

/// break-stmt ::= 'break' ('\n' | ';')
std::unique_ptr<BreakStmt> parseBreakStmt() {
    auto location = getCurrentLocation();
    consumeToken();
    parseStmtTerminator();
    return llvm::make_unique<BreakStmt>(location);
}

/// stmt ::= var-stmt | assign-stmt | compound-assign-stmt | return-stmt |
///          inc-stmt | dec-stmt | call-stmt | defer-stmt |
///          if-stmt | switch-stmt | while-stmt | for-stmt | break-stmt
std::unique_ptr<Stmt> parseStmt() {
    switch (currentToken()) {
        case RETURN: return parseReturnStmt();
        case LET: case VAR: return parseVarStmt();
        case DEFER: return parseDeferStmt();
        case IF: return parseIfStmt();
        case WHILE: return parseWhileStmt();
        case FOR: return parseForStmt();
        case SWITCH: return parseSwitchStmt();
        case BREAK: return parseBreakStmt();
        case UNDERSCORE: {
            consumeToken();
            parse(ASSIGN);
            auto stmt = llvm::make_unique<ExprStmt>(parseExpr());
            parseStmtTerminator();
            return std::move(stmt);
        }
        default: break;
    }

    // If we're here, the statement starts with an expression.
    std::unique_ptr<Expr> expr = parseExpr();

    switch (currentToken()) {
        case INCREMENT: return parseIncrementStmt(std::move(expr));
        case DECREMENT: return parseDecrementStmt(std::move(expr));
        case ASSIGN: return parseAssignStmt(std::move(expr));
        default:
            if (currentToken().isCompoundAssignmentOperator()) {
                return parseCompoundAssignStmt(std::move(expr));
            }

            if (!expr->isCallExpr()) unexpectedToken(currentToken());
            return parseCallStmt(std::move(expr));
    }
}

std::vector<std::unique_ptr<Stmt>> parseStmtsUntil(Token end) {
    std::vector<std::unique_ptr<Stmt>> stmts;
    while (currentToken() != end)
        stmts.emplace_back(parseStmt());
    return stmts;
}

std::vector<std::unique_ptr<Stmt>> parseStmtsUntilOneOf(Token end1, Token end2, Token end3) {
    std::vector<std::unique_ptr<Stmt>> stmts;
    while (currentToken() != end1 && currentToken() != end2  && currentToken() != end3)
        stmts.emplace_back(parseStmt());
    return stmts;
}

/// param-decl ::= id ':' type
ParamDecl parseParam() {
    auto name = parse(IDENTIFIER);
    parse(COLON);
    auto type = parseType();
    return ParamDecl(std::move(type), std::move(name.getString()), name.getLocation());
}

/// param-list ::= '(' params ')'
/// params ::= '' | non-empty-params
/// non-empty-params ::= param-decl | param-decl ',' non-empty-params
std::vector<ParamDecl> parseParamList() {
    parse(LPAREN);
    std::vector<ParamDecl> params;
    while (currentToken() != RPAREN) {
        params.emplace_back(parseParam());
        if (currentToken() != RPAREN) parse(COMMA);
    }
    parse(RPAREN);
    return params;
}

void parseGenericParamList(std::vector<GenericParamDecl>& genericParams) {
    parse(LT);
    while (true) {
        auto genericParamName = parse(IDENTIFIER);
        genericParams.emplace_back(genericParamName.getString(), genericParamName.getLocation());

        if (currentToken() == COLON) { // Generic type constraint.
            consumeToken();
            genericParams.back().addConstraint(parse(IDENTIFIER).getString());
            // TODO: Add support for multiple generic type constraints.
        }

        if (currentToken() == GT) break;
        parse(COMMA);
    }
    parse(GT);
}

/// function-proto ::= 'func' id param-list ('->' type)?
/// generic-function-proto ::= 'func' id generic-param-list param-list ('->' type)?
/// generic-param-list ::= '<' generic-param-decls '>'
/// generic-param-decls ::= id | id ',' generic-param-decls
std::unique_ptr<FunctionDecl> parseFunctionProto(TypeDecl* receiverTypeDecl) {
    ASSERT(currentToken() == FUNC);
    consumeToken();

    bool isValidFunctionName =
        currentToken() == IDENTIFIER ||
        currentToken().isOverloadable() ||
        (currentToken() == LBRACKET && lookAhead(1) == RBRACKET);

    if (!isValidFunctionName)
        unexpectedToken(currentToken(), {}, "as function name");

    SourceLocation nameLocation = getCurrentLocation();
    llvm::StringRef name;
    if (currentToken() == IDENTIFIER) {
        name = consumeToken().getString();
    } else if (currentToken() == LBRACKET) {
        consumeToken();
        parse(RBRACKET);
        name = "[]";
    } else if (receiverTypeDecl) {
        error(nameLocation, "operator functions other than subscript must be non-member functions");
    } else {
        name = toString(consumeToken().getKind());
    }

    std::vector<GenericParamDecl> genericParams;
    if (currentToken() == LT) {
        parseGenericParamList(genericParams);
    }

    auto params = parseParamList();

    Type returnType = Type::getVoid();
    if (currentToken() == RARROW) {
        consumeToken();
        returnType = parseType();
        while (currentToken() == COMMA) {
            consumeToken();
            returnType.appendType(parseType());
        }
    }

    FunctionProto proto(std::move(name), std::move(params), std::move(returnType),
                        std::move(genericParams), false);

    if (receiverTypeDecl) {
        return llvm::make_unique<MethodDecl>(std::move(proto), *receiverTypeDecl, nameLocation);
    } else {
        return llvm::make_unique<FunctionDecl>(std::move(proto), *currentModule, nameLocation);
    }
}

/// function-decl ::= function-proto '{' stmt* '}'
std::unique_ptr<FunctionDecl> parseFunctionDecl(TypeDecl* receiverTypeDecl, bool requireBody = true) {
    auto decl = parseFunctionProto(receiverTypeDecl);
    if (requireBody || currentToken() == LBRACE) {
        parse(LBRACE);
        decl->setBody(std::make_shared<std::vector<std::unique_ptr<Stmt>>>(parseStmtsUntil(RBRACE)));
        parse(RBRACE);
    }
    return decl;
}

/// extern-function-decl ::= 'extern' function-proto ('\n' | ';')
std::unique_ptr<FunctionDecl> parseExternFunctionDecl() {
    ASSERT(currentToken() == EXTERN);
    consumeToken();
    auto decl = parseFunctionProto(/* receiverTypeDecl */ nullptr);
    parseStmtTerminator();
    return decl;
}

/// init-decl ::= 'init' param-list '{' stmt* '}'
std::unique_ptr<InitDecl> parseInitDecl(TypeDecl& receiverTypeDecl) {
    auto initLocation = parse(INIT).getLocation();
    auto params = parseParamList();
    parse(LBRACE);
    auto body = std::make_shared<std::vector<std::unique_ptr<Stmt>>>(parseStmtsUntil(RBRACE));
    parse(RBRACE);
    return llvm::make_unique<InitDecl>(receiverTypeDecl, std::move(params), std::move(body),
                                       initLocation);
}

/// deinit-decl ::= 'deinit' '(' ')' '{' stmt* '}'
std::unique_ptr<DeinitDecl> parseDeinitDecl(TypeDecl& receiverTypeDecl) {
    auto deinitLocation = parse(DEINIT).getLocation();
    parse(LPAREN);
    auto expectedRParenLocation = getCurrentLocation();
    if (consumeToken() != RPAREN) error(expectedRParenLocation, "deinitializers cannot have parameters");
    parse(LBRACE);
    auto body = std::make_shared<std::vector<std::unique_ptr<Stmt>>>(parseStmtsUntil(RBRACE));
    parse(RBRACE);
    return llvm::make_unique<DeinitDecl>(receiverTypeDecl, std::move(body), deinitLocation);
}

/// field-decl ::= ('let' | 'var') id ':' type ('\n' | ';')
FieldDecl parseFieldDecl(TypeDecl& typeDecl) {
    expect({ LET, VAR }, "in field declaration");
    bool isMutable = consumeToken() == VAR;
    auto name = parse(IDENTIFIER);

    parse(COLON);
    auto typeLocation = getCurrentLocation();
    Type type = parseType();
    if (type.isMutable()) error(typeLocation, "type specifier cannot specify mutability");
    type.setMutable(isMutable);

    parseStmtTerminator();
    return FieldDecl(type, std::move(name.getString()), typeDecl, name.getLocation());
}

/// type-decl ::= ('class' | 'struct' | 'interface') id generic-param-list? '{' member-decl* '}'
/// member-decl ::= field-decl | function-decl
std::unique_ptr<TypeDecl> parseTypeDecl() {
    TypeTag tag;
    switch (consumeToken()) {
        case CLASS: tag = TypeTag::Class; break;
        case STRUCT: tag = TypeTag::Struct; break;
        case INTERFACE: tag = TypeTag::Interface; break;
        default: llvm_unreachable("invalid token");
    }

    auto name = parse(IDENTIFIER);

    std::vector<GenericParamDecl> genericParams;
    if (currentToken() == LT) {
        parseGenericParamList(genericParams);
    }

    auto typeDecl = llvm::make_unique<TypeDecl>(tag, name.getString(), std::move(genericParams),
                                                *currentModule, name.getLocation());
    parse(LBRACE);

    while (currentToken() != RBRACE) {
        switch (currentToken()) {
            case MUTATING:
                consumeToken();
                expect(FUNC, "after 'mutating'");
                LLVM_FALLTHROUGH;
            case FUNC: {
                bool isMutating = lookAhead(-1) == MUTATING;
                auto requireBody = tag != TypeTag::Interface;
                auto methodDecl = cast_unique_ptr<MethodDecl>(parseFunctionDecl(typeDecl.get(), requireBody));
                methodDecl->setMutating(isMutating);
                typeDecl->addMethod(std::move(methodDecl));
                break;
            }
            case INIT:
                typeDecl->addMethod(parseInitDecl(*typeDecl));
                break;
            case DEINIT:
                typeDecl->addMethod(parseDeinitDecl(*typeDecl));
                break;
            case LET: case VAR:
                typeDecl->addField(parseFieldDecl(*typeDecl));
                break;
            default:
                unexpectedToken(currentToken());
        }
    }

    consumeToken();
    return typeDecl;
}

/// import-decl ::= 'import' string-literal ('\n' | ';')
std::unique_ptr<ImportDecl> parseImportDecl() {
    ASSERT(currentToken() == IMPORT);
    consumeToken();
    expect(STRING_LITERAL, "after 'import'");
    auto target = parseStringLiteral();
    parseStmtTerminator("after 'import' declaration");
    return llvm::make_unique<ImportDecl>(target->getValue(), *currentModule, target->getLocation());
}

/// top-level-decl ::= function-decl | extern-function-decl | type-decl | import-decl | var-decl
std::unique_ptr<Decl> parseTopLevelDecl(const TypeChecker& typeChecker) {
    switch (currentToken()) {
        case FUNC: {
            auto decl = parseFunctionDecl(/* receiverTypeDecl */ nullptr);
            typeChecker.addToSymbolTable(*decl);
            return std::move(decl);
        }
        case EXTERN: {
            auto decl = parseExternFunctionDecl();
            typeChecker.addToSymbolTable(*decl);
            return std::move(decl);
        }
        case CLASS: case STRUCT: case INTERFACE: {
            auto decl = parseTypeDecl();
            typeChecker.addToSymbolTable(*decl);
            return std::move(decl);
        }
        case LET: case VAR: {
            auto decl = parseVarDecl();
            typeChecker.addToSymbolTable(*decl);
            return std::move(decl);
        }
        case IMPORT:
            return parseImportDecl();
        default:
            unexpectedToken(currentToken());
    }
}

void initParser(std::unique_ptr<llvm::MemoryBuffer> input) {
    initLexer(std::move(input));
    tokenBuffer.clear();
    currentTokenIndex = 0;
    tokenBuffer.emplace_back(lex());
}

SourceFile parse(std::unique_ptr<llvm::MemoryBuffer> input, Module& module) {
    std::string identifier = input->getBufferIdentifier();
    initParser(std::move(input));
    std::vector<std::unique_ptr<Decl>> topLevelDecls;
    SourceFile sourceFile(identifier);
    TypeChecker typeChecker(&module, &sourceFile);

    while (currentToken() != NO_TOKEN) {
        topLevelDecls.emplace_back(parseTopLevelDecl(typeChecker));
    }

    sourceFile.setDecls(std::move(topLevelDecls));
    return sourceFile;
}

}

void delta::parse(llvm::StringRef filePath, Module& module) {
    auto buffer = llvm::MemoryBuffer::getFile(filePath);
    if (!buffer) printErrorAndExit("no such file: '", filePath, "'");

    currentModule = &module;
    module.addSourceFile(::parse(std::move(*buffer), module));
}

std::unique_ptr<Expr> delta::parseExpr(std::unique_ptr<llvm::MemoryBuffer> input,
                                       Module& module) {
    initParser(std::move(input));
    currentModule = &module;
    return ::parseExpr();
}
