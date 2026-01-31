#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Use.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/IR/Type.h"
using namespace llvm;

#include <memory>
#include <iostream>
#include <fstream>
#include <unordered_set>
#include <algorithm>
#include <iterator>


typedef std::unordered_map<Function*, Function*> DispatchesToMap;

bool simplifyUnreachableUsages(BasicBlock *block);
Function* considerFunctionInlining(Function *prevFunction, Function *function, DispatchesToMap& dispatchesTo);
void startFunctionInlining(iterator_range<Module::iterator> functions);
bool containsOnlyCallInstrs(BasicBlock *block);


// clang++-17 main.cxx -o main `llvm-config-17 --cxxflags --ldflags --system-libs --libs all`
int main() {
    LLVMContext ctx;
    SMDiagnostic error;
    std::unique_ptr<Module> module = parseIRFile("test.ll", error, ctx);

    if (module == nullptr) {
        error.print("", errs());
        return 0;
    }

    startFunctionInlining(module->functions());

    std::unordered_set<BasicBlock*> unreachableBlocks;
    for (auto &function : module->functions()) {
        if (function.isDeclaration()) continue;
        for (auto it = function.begin(); it != function.end(); it++){
            BasicBlock &block = *it;
            if (isa<UnreachableInst>(block.back())){
                if (simplifyUnreachableUsages(&block)) {
                    unreachableBlocks.insert(&block);
                }
            }
        }
    }

    for (auto &block : unreachableBlocks) {
        block->removeFromParent();
    }

    std::error_code a;
    llvm::raw_fd_stream output("out.ll", a);
    module->print(output, nullptr);
    output.close();
    return 0;
}

bool simplifyUnreachableUsages(BasicBlock *block) {
    bool convertedAnyInstructions = false;

    for (User *user : block->users()) {
        TypeSwitch<Value*>(user)
            .Case<BranchInst>([&](BranchInst *branch) {
                if (branch->isConditional()) {
                    BasicBlock *successor = branch->getSuccessor(0);

                    // Successor that is not block will be our true successor:
                    if (successor == block) successor = branch->getSuccessor(1); 

                    // New unconditional br, remove the old one:
                    BranchInst* unconditionalInstr = BranchInst::Create(successor);
                    unconditionalInstr->insertAfter(branch);
                    branch->eraseFromParent();

                    convertedAnyInstructions = true;
                }
            })
            .Default([&](Value *other) {});
    }
    return convertedAnyInstructions;
}

/**
 * In addition to partial inlining, but not as aggressive as full.
 * This is geared more towards to monomorphized functions that
 * act as a dispatcher (eg: returns void, calls other methods).
 * 
*/
void startFunctionInlining(iterator_range<Module::iterator> iterator) {
    // F -> Final (where final cannot be a dispatcher).
    std::unordered_map<Function*, Function*> dispatchesTo;

    for (Function &function : iterator) {
        // Traverse through the calls downwards until we get to a non-dispatch:
        if (dispatchesTo.find(&function) == dispatchesTo.end()) {
            Function* nonDispatch = considerFunctionInlining(nullptr, &function, dispatchesTo);
        }
    }

    for (auto const& x : dispatchesTo) {
        Function *dispatch = x.first;
        Function *f = x.second;

        auto users = dispatch->users();

        // Get pointers to only calls. setCalledFunction will
        // modify the users iterator (which is why we split this)
        std::vector<CallInst*> calls;
        for (auto it = users.begin(); it != users.end(); ++it) {
            if (auto *call = dyn_cast<CallInst>(*it)) {
                calls.push_back(call);
            }
        }

        // All calls to dispatch are now replaced with a call to f.
        for (CallInst* call : calls) {
            call->setCalledFunction(f);
        }

        users = dispatch->users();
        if (std::distance(users.begin(), users.end()) == 0) {
            dispatch->removeFromParent();
        }
    }
}

Function* considerFunctionInlining(Function *prevFunction, Function *function, DispatchesToMap &dispatchesTo) {
    if (dispatchesTo.find(function) != dispatchesTo.end()) return dispatchesTo[function];

    if (
        function->getReturnType()->isVoidTy() &&
        function->size() == 1 && // (bblock size)
        containsOnlyCallInstrs(&function->front()) &&
        function->front().size() <= 2
        ) {

        // Iterate through calls:
        Function *nonDispatch = nullptr;
        for (Instruction &inst : function->front()) {
            CallInst *call = dyn_cast<CallInst>(&inst);
            if (!call || call->isInlineAsm()) continue;
            if (call->getCalledFunction() == nullptr) continue;
            nonDispatch = considerFunctionInlining(function, call->getCalledFunction(), dispatchesTo);
        }
        
        if (nonDispatch != nullptr) dispatchesTo[function] = nonDispatch;
        return nonDispatch;
    } else if (prevFunction != nullptr) {
        return function;
    }
    return nullptr;
}

/**
 * T/F for all instructions in block are Call Instructions
 * Explicitly checking for x is (CallInst, ReturnInst)
*/
bool containsOnlyCallInstrs(BasicBlock *block) {
    return std::all_of(block->begin(), block->end(), [](Instruction &instr) {
        return isa<CallInst>(instr) || isa<ReturnInst>(instr);
    });
}
