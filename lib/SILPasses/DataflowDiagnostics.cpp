//===-- DataflowDiagnostics.cpp - Emits diagnostics based on SIL analysis -===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2015 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
#include "swift/Subsystems.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/Diagnostics.h"
#include "swift/AST/Expr.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILLocation.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILVisitor.h"
using namespace swift;

template<typename...T, typename...U>
static void diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag,
              U &&...args) {
  Context.Diags.diagnose(loc,
                         diag, std::forward<U>(args)...);
}

static void diagnoseMissingReturn(const UnreachableInst *UI,
                                  ASTContext &Context) {
  const SILBasicBlock *BB = UI->getParent();
  const SILFunction *F = BB->getParent();
  SILLocation FLoc = F->getLocation();

  Type ResTy;

  if (auto *FD = FLoc.getAsASTNode<FuncDecl>()) {
    ResTy = FD->getResultType(Context);
    if (ResTy->isVoid())
      return;

    if (AnyFunctionType *T =
          FD->getFuncExpr()->getType()->castTo<AnyFunctionType>()) {
      if (T->isNoReturn())
        return;
    }
  } else {
    // FIXME: Not all closure types have the result type getter right
    // now.
    return;
  }

  // If the function does not return void, issue the diagnostic.
  SILLocation L = UI->getLoc();
  assert(L && ResTy);
  diagnose(Context,
           L.getEndSourceLoc(),
           diag::missing_return, ResTy);
}

static void diagnoseNonExhaustiveSwitch(const UnreachableInst *UI,
                                        ASTContext &Context) {
  SILLocation L = UI->getLoc();
  assert(L);
  diagnose(Context,
           L.getEndSourceLoc(),
           diag::non_exhaustive_switch);
}

static void diagnoseUnreachable(const SILInstruction *I,
                                ASTContext &Context) {
  if (auto *UI = dyn_cast<UnreachableInst>(I)){
    SILLocation L = UI->getLoc();

    // Invalid location means that the instruction has been generated by SIL
    // passes, such as DCE. FIXME: we might want to just introduce a separate
    // instruction kind, instead of keeping this invarient.
    if (!L.hasASTLocation())
      return;

    // The most common case of getting an unreachable instruction is a
    // missing return statement. In this case, we know that the instruction
    // location will be the enclosing function.
    if (L.isASTNode<AbstractFunctionDecl>()) {
      diagnoseMissingReturn(UI, Context);
      return;
    }

    // A non-exhaustive switch would also produce an unreachable instruction.
    if (L.isASTNode<SwitchStmt>()) {
      diagnoseNonExhaustiveSwitch(UI, Context);
      return;
    }
  }
}

static void diagnoseReturn(const SILInstruction *I, ASTContext &Context) {
  auto *TI = dyn_cast<TermInst>(I);
  if (!TI || !(isa<BranchInst>(TI) || isa<ReturnInst>(TI)))
    return;

  const SILBasicBlock *BB = TI->getParent();
  const SILFunction *F = BB->getParent();
  SILLocation FLoc = F->getLocation();

  if (auto *FD = FLoc.getAsASTNode<FuncDecl>()) {
    if (AnyFunctionType *T =
            FD->getFuncExpr()->getType()->castTo<AnyFunctionType>()) {

      // Warn if we reach a return inside a noreturn function.
      if (T->isNoReturn()) {
        SILLocation L = TI->getLoc();
        if (L.is<ReturnLocation>())
          diagnose(Context, L.getSourceLoc(), diag::return_from_noreturn);
        if (L.is<ImplicitReturnLocation>())
          diagnose(Context, L.getSourceLoc(), diag::return_from_noreturn);
      }
    }
  }
}

void swift::emitSILDataflowDiagnostics(const SILModule *M) {
  for (auto &Fn : *M)
    for (auto &BB : Fn)
      for (auto &I : BB) {
        diagnoseUnreachable(&I, M->getASTContext());
        diagnoseReturn(&I, M->getASTContext());
      }
}
