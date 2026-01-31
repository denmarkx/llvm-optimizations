#include "llvm/IRReader/IRReader.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/User.h"
#include "llvm/IR/Use.h"
#include "llvm/ADT/TypeSwitch.h"

#include <memory>
#include <iostream>
#include <fstream>
#include <unordered_set>
using namespace llvm;

bool simplifyUnreachableUsages(BasicBlock *block);

// clang++-17 main.cxx -o main `llvm-config-17 --cxxflags --ldflags --system-libs --libs all`
int main() {
    LLVMContext ctx;
    SMDiagnostic error;
    std::unique_ptr<Module> module = parseIRFile("test.ll", error, ctx);

    if (module == nullptr) {
        error.print("", errs());
        return 0;
    }

    std::unordered_set<BasicBlock*> unreachableBlocks;
    for (auto &function : module->functions()) {
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

