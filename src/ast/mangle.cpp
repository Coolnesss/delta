#include <llvm/ADT/ArrayRef.h>
#include "mangle.h"
#include "../ast/decl.h"

using namespace delta;

static void appendGenericArgs(std::string& mangled, llvm::ArrayRef<Type> genericArgs) {
    if (genericArgs.empty()) return;

    mangled += '<';
    for (const Type& genericArg : genericArgs) {
        mangled += genericArg.toString();
        if (&genericArg != &genericArgs.back()) mangled += ", ";
    }
    mangled += '>';
}

std::string delta::mangle(const FunctionLikeDecl& decl, llvm::ArrayRef<Type> typeGenericArgs,
                          llvm::ArrayRef<Type> functionGenericArgs) {
    std::string receiverTypeName = decl.getTypeDecl() ? decl.getTypeDecl()->getName() : "";
    appendGenericArgs(receiverTypeName, typeGenericArgs);
    return mangleFunctionDecl(receiverTypeName, decl.getName(), functionGenericArgs);
}

std::string delta::mangleFunctionDecl(llvm::StringRef receiverType, llvm::StringRef functionName,
                                      llvm::ArrayRef<Type> genericArgs) {
    std::string mangled;
    if (receiverType.empty()) {
        mangled = functionName.str();
    } else {
        mangled = receiverType.str() + "." + functionName.str();
    }
    appendGenericArgs(mangled, genericArgs);
    return mangled;
}

std::string delta::mangle(const InitDecl& decl, llvm::ArrayRef<Type> typeGenericArgs,
                          llvm::ArrayRef<Type> functionGenericArgs) {
    std::string typeName = decl.getTypeDecl()->getName();
    appendGenericArgs(typeName, typeGenericArgs);
    return mangleInitDecl(typeName, functionGenericArgs);
}

std::string delta::mangleInitDecl(llvm::StringRef typeName, llvm::ArrayRef<Type> genericArgs) {
    auto mangled = typeName.str() + ".init";
    appendGenericArgs(mangled, genericArgs);
    return mangled;
}

std::string delta::mangle(const DeinitDecl& decl, llvm::ArrayRef<Type> typeGenericArgs) {
    std::string typeName = decl.getTypeDecl()->getName();
    appendGenericArgs(typeName, typeGenericArgs);
    return mangleDeinitDecl(typeName);
}

std::string delta::mangleDeinitDecl(llvm::StringRef typeName) {
    return typeName.str() + ".deinit";
}

std::string delta::mangle(const TypeDecl& decl, llvm::ArrayRef<Type> genericArgs) {
    std::string mangled = decl.getName();
    appendGenericArgs(mangled, genericArgs);
    return mangled;
}
