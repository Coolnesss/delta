#include "repl.h"
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Format.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/LineEditor/LineEditor.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/Interpreter.h>
#include "../ast/expr.h"
#include "../ast/module.h"
#include "../irgen/irgen.h"
#include "../parser/parse.h"
#include "../sema/typecheck.h"
#include "../support/utility.h"

using namespace delta;

namespace {

void evaluate(llvm::StringRef line) {
    Module module("main");
    module.addSourceFile(SourceFile(llvm::StringRef()));

    IRGenerator irGenerator;
    irGenerator.setTypeChecker(TypeChecker(&module, &module.getSourceFiles().front()));

    std::unique_ptr<Expr> expr;
    try {
        expr = parseExpr(llvm::MemoryBuffer::getMemBuffer(line, "", false), module);
        irGenerator.getTypeChecker().typecheckExpr(*expr);
    } catch (const CompileError& error) {
        llvm::StringRef trimmed = line.ltrim();
        bool isComment = trimmed.size() >= 2 && trimmed[0] == '/' && trimmed[1] == '/';
        if (!trimmed.empty() && !isComment) error.print();
        return;
    }

    llvm::InitializeNativeTarget();
    auto irModule = llvm::make_unique<llvm::Module>("deltajit", irgen::getContext());
    auto& irModuleRef = *irModule;

    std::string error;
    llvm::ExecutionEngine* engine = llvm::EngineBuilder(std::move(irModule)).setErrorStr(&error).create();
    if (!error.empty()) llvm::outs() << error << '\n';

    llvm::FunctionType* functionType = llvm::FunctionType::get(irGenerator.toIR(expr->getType()), {}, false);
    llvm::Function* function = llvm::Function::Create(functionType, llvm::Function::ExternalLinkage,
                                                      "__anon_expr", &irModuleRef);
    irGenerator.getBuilder().SetInsertPoint(llvm::BasicBlock::Create(irgen::getContext(), "", function));
    irGenerator.getBuilder().CreateRet(irGenerator.codegenExpr(*expr));
    ASSERT(!llvm::verifyModule(irModuleRef, &llvm::errs()));

    llvm::GenericValue result = engine->runFunction(function, {});

    if (expr->getType().isInteger()) {
        result.IntVal.print(llvm::outs(), expr->getType().isSigned());
    } else if (expr->getType().isFloatingPoint()) {
        llvm::outs() << result.DoubleVal;
    } else if (expr->getType().isBool()) {
        llvm::outs() << (result.IntVal != 0 ? "true" : "false");
    } else {
        printErrorAndExit("unsupported result type");
    }

    llvm::outs() << '\n';
}

}

int delta::replMain() {
    llvm::LineEditor editor("", llvm::LineEditor::getDefaultHistoryPath("delta-repl"));
    int lineNumber = 0;
    std::string prompt;

    while (true) {
        llvm::raw_string_ostream(prompt) << llvm::format_decimal(lineNumber, 3) << "> ";
        editor.setPrompt(prompt);

        auto line = editor.readLine();
        if (!line) break;

        editor.saveHistory();
        evaluate(*line);
        lineNumber++;
        prompt.clear();
    }

    return 0;
}
