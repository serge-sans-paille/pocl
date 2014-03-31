// LLVM function pass that adds implicit barriers to loops if it sees
// beneficial.
// 
// Copyright (c) 2012-2013 Pekka Jääskeläinen / TUT
// 
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#include "config.h"
#include "ImplicitLoopBarriers.h"
#include "Barrier.h"
#include "Workgroup.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#if (defined LLVM_3_1 or defined LLVM_3_2)
#include "llvm/Constants.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#else
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#endif
#if not (defined LLVM_3_2 or defined LLVM_3_3 or defined LLVM_3_4)
#include "llvm/IR/Dominators.h"
#endif

#include "VariableUniformityAnalysis.h"

#include <iostream>

//#define DEBUG_ILOOP_BARRIERS

using namespace llvm;
using namespace pocl;

namespace {
  static
  RegisterPass<ImplicitLoopBarriers> X("implicit-loop-barriers",
                                       "Adds implicit barriers to loops");
}

char ImplicitLoopBarriers::ID = 0;

void
ImplicitLoopBarriers::getAnalysisUsage(AnalysisUsage &AU) const
{
#if (defined LLVM_3_2 or defined LLVM_3_3 or defined LLVM_3_4)
  AU.addRequired<DominatorTree>();
  AU.addPreserved<DominatorTree>();
#else
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addPreserved<DominatorTreeWrapperPass>();
#endif
  AU.addRequired<VariableUniformityAnalysis>();
  AU.addPreserved<VariableUniformityAnalysis>();
}

bool
ImplicitLoopBarriers::runOnLoop(Loop *L, LPPassManager &LPM)
{
  if (!Workgroup::isKernelToProcess(*L->getHeader()->getParent()))
    return false;

  return ProcessLoop(L, LPM);
}


/**
 * Adds a barrier to the first BB of each loop.
 *
 * Note: it's not safe to do this in case the loop is not executed
 * by all work items. Therefore this is not enabled by default.
 */
bool
ImplicitLoopBarriers::ProcessLoop(Loop *L, LPPassManager &LPM)
{

  bool isBLoop = false;
  for (Loop::block_iterator i = L->block_begin(), e = L->block_end();
       i != e && !isBLoop; ++i) {
    for (BasicBlock::iterator j = (*i)->begin(), e = (*i)->end();
         j != e; ++j) {
      if (isa<Barrier>(j)) {
          isBLoop = true;
          break;
      }
    }
  }
  if (isBLoop) return false;

  return AddInnerLoopBarrier(L, LPM);
}

/**
 * Adds a barrier to the beginning of the loop body to force its treatment 
 * similarly to a loop with work-group barriers.
 *
 * This allows parallelizing work-items across the work-group per kernel
 * for-loop iteration, potentially leading to easier horizontal vectorization.
 * The idea is similar to loop switching where the work-item loop is 
 * switched with the kernel for-loop.
 *
 * We need to make sure it is legal to add the barrier, though. The
 * OpenCL barrier semantics require either all or none of the WIs to
 * reach the barrier at each iteration. This is satisfied at least when
 *
 * a) loop exit condition does not depend on the WI and 
 * b) all or none of the WIs always enter the loop
 */
bool
ImplicitLoopBarriers::AddInnerLoopBarrier(llvm::Loop *L, llvm::LPPassManager &LPM) {

  /* Only add barriers to the innermost loops. */

  if (L->getSubLoops().size() > 0)
    return false;

#ifdef DEBUG_ILOOP_BARRIERS
  std::cerr << "### trying to add a loop barrier to force horizontal parallelization" 
            << std::endl;
#endif

  BasicBlock *brexit = L->getExitingBlock();
  if (brexit == NULL) return false; /* Multiple exit points */

  llvm::BasicBlock *loopEntry = L->getHeader();
  if (loopEntry == NULL) return false; /* Multiple entries blocks? */

  llvm::Function *f = brexit->getParent();

  VariableUniformityAnalysis &VUA = 
    getAnalysis<VariableUniformityAnalysis>();

  /* Check if the whole loop construct is executed by all or none of the
     work-items. */
  if (!VUA.isUniform(f, loopEntry)) {
#ifdef DEBUG_ILOOP_BARRIERS
    std::cerr << "### the loop is not uniform because loop entry '"
              << loopEntry->getName().str() << "' is not uniform" << std::endl;
    
#endif
    return false;
  }

  /* Check the branch condition predicate. If it is uniform, we know the loop 
     is  executed the same number of times for all WIs. */
  llvm::BranchInst *br = dyn_cast<llvm::BranchInst>(brexit->getTerminator());  
  if (br && br->isConditional() &&
      VUA.isUniform(f, br->getCondition())) {

    /* Add a barrier both to the beginning of the entry and to the very end
       to nicely isolate the parallel region. */
    Barrier::Create(brexit->getTerminator());   
    Barrier::Create(loopEntry->getFirstNonPHI());

#ifdef DEBUG_ILOOP_BARRIERS
    std::cerr << "### added an inner-loop barrier to the loop" << std::endl << std::endl;
#endif
    return true;
  } else {
#ifdef DEBUG_ILOOP_BARRIERS
    if (br && br->isConditional() && !VUA.isUniform(f, br->getCondition())) {
      std::cerr << "### loop condition not uniform" << std::endl;
      br->getCondition()->dump();
    }
#endif

  }

#ifdef DEBUG_ILOOP_BARRIERS
  std::cerr << "### cannot add an inner-loop barrier to the loop" << std::endl << std::endl;
#endif
  
  return false;
}
