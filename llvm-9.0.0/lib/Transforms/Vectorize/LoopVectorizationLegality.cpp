//===- LoopVectorizationLegality.cpp --------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file provides loop vectorization legality analysis. Original code
// resided in LoopVectorize.cpp for a long time.
//
// At this point, it is implemented as a utility class, not as an analysis
// pass. It should be easy to create an analysis pass around it if there
// is a need (but D45420 needs to happen first).
//
#include "llvm/Transforms/Vectorize/LoopVectorizationLegality.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/IntrinsicInst.h"

using namespace llvm;

#define LV_NAME "loop-vectorize"
#define DEBUG_TYPE LV_NAME

extern cl::opt<bool> EnableVPlanPredication;

static cl::opt<bool>
    EnableIfConversion("enable-if-conversion", cl::init(true), cl::Hidden,
                       cl::desc("Enable if-conversion during vectorization."));

static cl::opt<unsigned> PragmaVectorizeMemoryCheckThreshold(
    "pragma-vectorize-memory-check-threshold", cl::init(128), cl::Hidden,
    cl::desc("The maximum allowed number of runtime memory checks with a "
             "vectorize(enable) pragma."));

static cl::opt<unsigned> VectorizeSCEVCheckThreshold(
    "vectorize-scev-check-threshold", cl::init(16), cl::Hidden,
    cl::desc("The maximum number of SCEV checks allowed."));

static cl::opt<unsigned> PragmaVectorizeSCEVCheckThreshold(
    "pragma-vectorize-scev-check-threshold", cl::init(128), cl::Hidden,
    cl::desc("The maximum number of SCEV checks allowed with a "
             "vectorize(enable) pragma"));

/// Maximum vectorization interleave count.
static const unsigned MaxInterleaveFactor = 16;

namespace llvm {

#ifndef NDEBUG
static void debugVectorizationFailure(const StringRef DebugMsg,
    Instruction *I) {
  dbgs() << "LV: Not vectorizing: " << DebugMsg;
  if (I != nullptr)
    dbgs() << " " << *I;
  else
    dbgs() << '.';
  dbgs() << '\n';
}
#endif

OptimizationRemarkAnalysis createLVMissedAnalysis(const char *PassName,
                                                  StringRef RemarkName,
                                                  Loop *TheLoop,
                                                  Instruction *I) {
  Value *CodeRegion = TheLoop->getHeader();
  DebugLoc DL = TheLoop->getStartLoc();

  if (I) {
    CodeRegion = I->getParent();
    // If there is no debug location attached to the instruction, revert back to
    // using the loop's.
    if (I->getDebugLoc())
      DL = I->getDebugLoc();
  }

  OptimizationRemarkAnalysis R(PassName, RemarkName, DL, CodeRegion);
  R << "loop not vectorized: ";
  return R;
}

bool LoopVectorizeHints::Hint::validate(unsigned Val) {
  switch (Kind) {
  case HK_WIDTH:
    return isPowerOf2_32(Val) && Val <= VectorizerParams::MaxVectorWidth;
  case HK_UNROLL:
    return isPowerOf2_32(Val) && Val <= MaxInterleaveFactor;
  case HK_FORCE:
    return (Val <= 1);
  case HK_ISVECTORIZED:
    return (Val == 0 || Val == 1);
  }
  return false;
}

LoopVectorizeHints::LoopVectorizeHints(const Loop *L,
                                       bool InterleaveOnlyWhenForced,
                                       OptimizationRemarkEmitter &ORE)
    : Width("vectorize.width", VectorizerParams::VectorizationFactor, HK_WIDTH),
      Interleave("interleave.count", InterleaveOnlyWhenForced, HK_UNROLL),
      Force("vectorize.enable", FK_Undefined, HK_FORCE),
      IsVectorized("isvectorized", 0, HK_ISVECTORIZED), TheLoop(L), ORE(ORE) {
  // Populate values with existing loop metadata.
  getHintsFromMetadata();

  // force-vector-interleave overrides DisableInterleaving.
  if (VectorizerParams::isInterleaveForced())
    Interleave.Value = VectorizerParams::VectorizationInterleave;

  if (IsVectorized.Value != 1)
    // If the vectorization width and interleaving count are both 1 then
    // consider the loop to have been already vectorized because there's
    // nothing more that we can do.
    IsVectorized.Value = Width.Value == 1 && Interleave.Value == 1;
  LLVM_DEBUG(if (InterleaveOnlyWhenForced && Interleave.Value == 1) dbgs()
             << "LV: Interleaving disabled by the pass manager\n");
}

void LoopVectorizeHints::setAlreadyVectorized() {
  LLVMContext &Context = TheLoop->getHeader()->getContext();

  MDNode *IsVectorizedMD = MDNode::get(
      Context,
      {MDString::get(Context, "llvm.loop.isvectorized"),
       ConstantAsMetadata::get(ConstantInt::get(Context, APInt(32, 1)))});
  MDNode *LoopID = TheLoop->getLoopID();
  MDNode *NewLoopID =
      makePostTransformationMetadata(Context, LoopID,
                                     {Twine(Prefix(), "vectorize.").str(),
                                      Twine(Prefix(), "interleave.").str()},
                                     {IsVectorizedMD});
  TheLoop->setLoopID(NewLoopID);

  // Update internal cache.
  IsVectorized.Value = 1;
}

bool LoopVectorizeHints::allowVectorization(
    Function *F, Loop *L, bool VectorizeOnlyWhenForced) const {
  if (getForce() == LoopVectorizeHints::FK_Disabled) {
    LLVM_DEBUG(dbgs() << "LV: Not vectorizing: #pragma vectorize disable.\n");
    emitRemarkWithHints();
    return false;
  }

  if (VectorizeOnlyWhenForced && getForce() != LoopVectorizeHints::FK_Enabled) {
    LLVM_DEBUG(dbgs() << "LV: Not vectorizing: No #pragma vectorize enable.\n");
    emitRemarkWithHints();
    return false;
  }

  if (getIsVectorized() == 1) {
    LLVM_DEBUG(dbgs() << "LV: Not vectorizing: Disabled/already vectorized.\n");
    // FIXME: Add interleave.disable metadata. This will allow
    // vectorize.disable to be used without disabling the pass and errors
    // to differentiate between disabled vectorization and a width of 1.
    ORE.emit([&]() {
      return OptimizationRemarkAnalysis(vectorizeAnalysisPassName(),
                                        "AllDisabled", L->getStartLoc(),
                                        L->getHeader())
             << "loop not vectorized: vectorization and interleaving are "
                "explicitly disabled, or the loop has already been "
                "vectorized";
    });
    return false;
  }

  return true;
}

void LoopVectorizeHints::emitRemarkWithHints() const {
  using namespace ore;

  ORE.emit([&]() {
    if (Force.Value == LoopVectorizeHints::FK_Disabled)
      return OptimizationRemarkMissed(LV_NAME, "MissedExplicitlyDisabled",
                                      TheLoop->getStartLoc(),
                                      TheLoop->getHeader())
             << "loop not vectorized: vectorization is explicitly disabled";
    else {
      OptimizationRemarkMissed R(LV_NAME, "MissedDetails",
                                 TheLoop->getStartLoc(), TheLoop->getHeader());
      R << "loop not vectorized";
      if (Force.Value == LoopVectorizeHints::FK_Enabled) {
        R << " (Force=" << NV("Force", true);
        if (Width.Value != 0)
          R << ", Vector Width=" << NV("VectorWidth", Width.Value);
        if (Interleave.Value != 0)
          R << ", Interleave Count=" << NV("InterleaveCount", Interleave.Value);
        R << ")";
      }
      return R;
    }
  });
}

const char *LoopVectorizeHints::vectorizeAnalysisPassName() const {
  if (getWidth() == 1)
    return LV_NAME;
  if (getForce() == LoopVectorizeHints::FK_Disabled)
    return LV_NAME;
  if (getForce() == LoopVectorizeHints::FK_Undefined && getWidth() == 0)
    return LV_NAME;
  return OptimizationRemarkAnalysis::AlwaysPrint;
}

void LoopVectorizeHints::getHintsFromMetadata() {
  MDNode *LoopID = TheLoop->getLoopID();
  if (!LoopID)
    return;

  // First operand should refer to the loop id itself.
  assert(LoopID->getNumOperands() > 0 && "requires at least one operand");
  assert(LoopID->getOperand(0) == LoopID && "invalid loop id");

  for (unsigned i = 1, ie = LoopID->getNumOperands(); i < ie; ++i) {
    const MDString *S = nullptr;
    SmallVector<Metadata *, 4> Args;

    // The expected hint is either a MDString or a MDNode with the first
    // operand a MDString.
    if (const MDNode *MD = dyn_cast<MDNode>(LoopID->getOperand(i))) {
      if (!MD || MD->getNumOperands() == 0)
        continue;
      S = dyn_cast<MDString>(MD->getOperand(0));
      for (unsigned i = 1, ie = MD->getNumOperands(); i < ie; ++i)
        Args.push_back(MD->getOperand(i));
    } else {
      S = dyn_cast<MDString>(LoopID->getOperand(i));
      assert(Args.size() == 0 && "too many arguments for MDString");
    }

    if (!S)
      continue;

    // Check if the hint starts with the loop metadata prefix.
    StringRef Name = S->getString();
    if (Args.size() == 1)
      setHint(Name, Args[0]);
  }
}

void LoopVectorizeHints::setHint(StringRef Name, Metadata *Arg) {
  if (!Name.startswith(Prefix()))
    return;
  Name = Name.substr(Prefix().size(), StringRef::npos);

  const ConstantInt *C = mdconst::dyn_extract<ConstantInt>(Arg);
  if (!C)
    return;
  unsigned Val = C->getZExtValue();

  Hint *Hints[] = {&Width, &Interleave, &Force, &IsVectorized};
  for (auto H : Hints) {
    if (Name == H->Name) {
      if (H->validate(Val))
        H->Value = Val;
      else
        LLVM_DEBUG(dbgs() << "LV: ignoring invalid hint '" << Name << "'\n");
      break;
    }
  }
}

bool LoopVectorizationRequirements::doesNotMeet(
    Function *F, Loop *L, const LoopVectorizeHints &Hints) {
  const char *PassName = Hints.vectorizeAnalysisPassName();
  bool Failed = false;
  if (UnsafeAlgebraInst && !Hints.allowReordering()) {
    ORE.emit([&]() {
      return OptimizationRemarkAnalysisFPCommute(
                 PassName, "CantReorderFPOps", UnsafeAlgebraInst->getDebugLoc(),
                 UnsafeAlgebraInst->getParent())
             << "loop not vectorized: cannot prove it is safe to reorder "
                "floating-point operations";
    });
    Failed = true;
  }

  // Test if runtime memcheck thresholds are exceeded.
  bool PragmaThresholdReached =
      NumRuntimePointerChecks > PragmaVectorizeMemoryCheckThreshold;
  bool ThresholdReached =
      NumRuntimePointerChecks > VectorizerParams::RuntimeMemoryCheckThreshold;
  if ((ThresholdReached && !Hints.allowReordering()) ||
      PragmaThresholdReached) {
    ORE.emit([&]() {
      return OptimizationRemarkAnalysisAliasing(PassName, "CantReorderMemOps",
                                                L->getStartLoc(),
                                                L->getHeader())
             << "loop not vectorized: cannot prove it is safe to reorder "
                "memory operations";
    });
    LLVM_DEBUG(dbgs() << "LV: Too many memory checks needed.\n");
    Failed = true;
  }

  return Failed;
}

// Return true if the inner loop \p Lp is uniform with regard to the outer loop
// \p OuterLp (i.e., if the outer loop is vectorized, all the vector lanes
// executing the inner loop will execute the same iterations). This check is
// very constrained for now but it will be relaxed in the future. \p Lp is
// considered uniform if it meets all the following conditions:
//   1) it has a canonical IV (starting from 0 and with stride 1),
//   2) its latch terminator is a conditional branch and,
//   3) its latch condition is a compare instruction whose operands are the
//      canonical IV and an OuterLp invariant.
// This check doesn't take into account the uniformity of other conditions not
// related to the loop latch because they don't affect the loop uniformity.
//
// NOTE: We decided to keep all these checks and its associated documentation
// together so that we can easily have a picture of the current supported loop
// nests. However, some of the current checks don't depend on \p OuterLp and
// would be redundantly executed for each \p Lp if we invoked this function for
// different candidate outer loops. This is not the case for now because we
// don't currently have the infrastructure to evaluate multiple candidate outer
// loops and \p OuterLp will be a fixed parameter while we only support explicit
// outer loop vectorization. It's also very likely that these checks go away
// before introducing the aforementioned infrastructure. However, if this is not
// the case, we should move the \p OuterLp independent checks to a separate
// function that is only executed once for each \p Lp.
static bool isUniformLoop(Loop *Lp, Loop *OuterLp) {
  assert(Lp->getLoopLatch() && "Expected loop with a single latch.");

  // If Lp is the outer loop, it's uniform by definition.
  if (Lp == OuterLp)
    return true;
  assert(OuterLp->contains(Lp) && "OuterLp must contain Lp.");

  // 1.
  PHINode *IV = Lp->getCanonicalInductionVariable();
  if (!IV) {
    LLVM_DEBUG(dbgs() << "LV: Canonical IV not found.\n");
    return false;
  }

  // 2.
  BasicBlock *Latch = Lp->getLoopLatch();
  auto *LatchBr = dyn_cast<BranchInst>(Latch->getTerminator());
  if (!LatchBr || LatchBr->isUnconditional()) {
    LLVM_DEBUG(dbgs() << "LV: Unsupported loop latch branch.\n");
    return false;
  }

  // 3.
  auto *LatchCmp = dyn_cast<CmpInst>(LatchBr->getCondition());
  if (!LatchCmp) {
    LLVM_DEBUG(
        dbgs() << "LV: Loop latch condition is not a compare instruction.\n");
    return false;
  }

  Value *CondOp0 = LatchCmp->getOperand(0);
  Value *CondOp1 = LatchCmp->getOperand(1);
  Value *IVUpdate = IV->getIncomingValueForBlock(Latch);
  if (!(CondOp0 == IVUpdate && OuterLp->isLoopInvariant(CondOp1)) &&
      !(CondOp1 == IVUpdate && OuterLp->isLoopInvariant(CondOp0))) {
    LLVM_DEBUG(dbgs() << "LV: Loop latch condition is not uniform.\n");
    return false;
  }

  return true;
}

// Return true if \p Lp and all its nested loops are uniform with regard to \p
// OuterLp.
static bool isUniformLoopNest(Loop *Lp, Loop *OuterLp) {
  if (!isUniformLoop(Lp, OuterLp))
    return false;

  // Check if nested loops are uniform.
  for (Loop *SubLp : *Lp)
    if (!isUniformLoopNest(SubLp, OuterLp))
      return false;

  return true;
}

/// Check whether it is safe to if-convert this phi node.
///
/// Phi nodes with constant expressions that can trap are not safe to if
/// convert.
static bool canIfConvertPHINodes(BasicBlock *BB) {
  for (PHINode &Phi : BB->phis()) {
    for (Value *V : Phi.incoming_values())
      if (auto *C = dyn_cast<Constant>(V))
        if (C->canTrap())
          return false;
  }
  return true;
}

static Type *convertPointerToIntegerType(const DataLayout &DL, Type *Ty) {
  if (Ty->isPointerTy())
    return DL.getIntPtrType(Ty);

  // It is possible that char's or short's overflow when we ask for the loop's
  // trip count, work around this by changing the type size.
  if (Ty->getScalarSizeInBits() < 32)
    return Type::getInt32Ty(Ty->getContext());

  return Ty;
}

static Type *getWiderType(const DataLayout &DL, Type *Ty0, Type *Ty1) {
  Ty0 = convertPointerToIntegerType(DL, Ty0);
  Ty1 = convertPointerToIntegerType(DL, Ty1);
  if (Ty0->getScalarSizeInBits() > Ty1->getScalarSizeInBits())
    return Ty0;
  return Ty1;
}

/// Check that the instruction has outside loop users and is not an
/// identified reduction variable.
static bool hasOutsideLoopUser(const Loop *TheLoop, Instruction *Inst,
                               SmallPtrSetImpl<Value *> &AllowedExit) {
  // Reductions, Inductions and non-header phis are allowed to have exit users. All
  // other instructions must not have external users.
  if (!AllowedExit.count(Inst))
    // Check that all of the users of the loop are inside the BB.
    for (User *U : Inst->users()) {
      Instruction *UI = cast<Instruction>(U);
      // This user may be a reduction exit value.
      if (!TheLoop->contains(UI)) {
        LLVM_DEBUG(dbgs() << "LV: Found an outside user for : " << *UI << '\n');
        return true;
      }
    }
  return false;
}

int LoopVectorizationLegality::isConsecutivePtr(Value *Ptr) {
  const ValueToValueMap &Strides =
      getSymbolicStrides() ? *getSymbolicStrides() : ValueToValueMap();

  int Stride = getPtrStride(PSE, Ptr, TheLoop, Strides, true, false);
  if (Stride == 1 || Stride == -1)
    return Stride;
  return 0;
}

bool LoopVectorizationLegality::isUniform(Value *V) {
  return LAI->isUniform(V);
}

void LoopVectorizationLegality::reportVectorizationFailure(
    const StringRef DebugMsg, const StringRef OREMsg,
    const StringRef ORETag, Instruction *I) const {
  LLVM_DEBUG(debugVectorizationFailure(DebugMsg, I));
  ORE->emit(createLVMissedAnalysis(Hints->vectorizeAnalysisPassName(),
      ORETag, TheLoop, I) << OREMsg);
}

bool LoopVectorizationLegality::canVectorizeOuterLoop() {
  assert(!TheLoop->empty() && "We are not vectorizing an outer loop.");
  // Store the result and return it at the end instead of exiting early, in case
  // allowExtraAnalysis is used to report multiple reasons for not vectorizing.
  bool Result = true;
  bool DoExtraAnalysis = ORE->allowExtraAnalysis(DEBUG_TYPE);

  for (BasicBlock *BB : TheLoop->blocks()) {
    // Check whether the BB terminator is a BranchInst. Any other terminator is
    // not supported yet.
    auto *Br = dyn_cast<BranchInst>(BB->getTerminator());
    if (!Br) {
      reportVectorizationFailure("Unsupported basic block terminator",
          "loop control flow is not understood by vectorizer",
          "CFGNotUnderstood");
      if (DoExtraAnalysis)
        Result = false;
      else
        return false;
    }

    // Check whether the BranchInst is a supported one. Only unconditional
    // branches, conditional branches with an outer loop invariant condition or
    // backedges are supported.
    // FIXME: We skip these checks when VPlan predication is enabled as we
    // want to allow divergent branches. This whole check will be removed
    // once VPlan predication is on by default.
    if (!EnableVPlanPredication && Br && Br->isConditional() &&
        !TheLoop->isLoopInvariant(Br->getCondition()) &&
        !LI->isLoopHeader(Br->getSuccessor(0)) &&
        !LI->isLoopHeader(Br->getSuccessor(1))) {
      reportVectorizationFailure("Unsupported conditional branch",
          "loop control flow is not understood by vectorizer",
          "CFGNotUnderstood");
      if (DoExtraAnalysis)
        Result = false;
      else
        return false;
    }
  }

  // Check whether inner loops are uniform. At this point, we only support
  // simple outer loops scenarios with uniform nested loops.
  if (!isUniformLoopNest(TheLoop /*loop nest*/,
                         TheLoop /*context outer loop*/)) {
    reportVectorizationFailure("Outer loop contains divergent loops",
        "loop control flow is not understood by vectorizer",
        "CFGNotUnderstood");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  // Check whether we are able to set up outer loop induction.
  if (!setupOuterLoopInductions()) {
    reportVectorizationFailure("Unsupported outer loop Phi(s)",
                               "Unsupported outer loop Phi(s)",
                               "UnsupportedPhi");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  return Result;
}

void LoopVectorizationLegality::addInductionPhi(
    PHINode *Phi, const InductionDescriptor &ID,
    SmallPtrSetImpl<Value *> &AllowedExit) {
  Inductions[Phi] = ID;

  // In case this induction also comes with casts that we know we can ignore
  // in the vectorized loop body, record them here. All casts could be recorded
  // here for ignoring, but suffices to record only the first (as it is the
  // only one that may bw used outside the cast sequence).
  const SmallVectorImpl<Instruction *> &Casts = ID.getCastInsts();
  if (!Casts.empty())
    InductionCastsToIgnore.insert(*Casts.begin());

  Type *PhiTy = Phi->getType();
  const DataLayout &DL = Phi->getModule()->getDataLayout();

  // Get the widest type.
  if (!PhiTy->isFloatingPointTy()) {
    if (!WidestIndTy)
      WidestIndTy = convertPointerToIntegerType(DL, PhiTy);
    else
      WidestIndTy = getWiderType(DL, PhiTy, WidestIndTy);
  }

  // Int inductions are special because we only allow one IV.
  if (ID.getKind() == InductionDescriptor::IK_IntInduction &&
      ID.getConstIntStepValue() && ID.getConstIntStepValue()->isOne() &&
      isa<Constant>(ID.getStartValue()) &&
      cast<Constant>(ID.getStartValue())->isNullValue()) {

    // Use the phi node with the widest type as induction. Use the last
    // one if there are multiple (no good reason for doing this other
    // than it is expedient). We've checked that it begins at zero and
    // steps by one, so this is a canonical induction variable.
    if (!PrimaryInduction || PhiTy == WidestIndTy)
      PrimaryInduction = Phi;
  }

  // Both the PHI node itself, and the "post-increment" value feeding
  // back into the PHI node may have external users.
  // We can allow those uses, except if the SCEVs we have for them rely
  // on predicates that only hold within the loop, since allowing the exit
  // currently means re-using this SCEV outside the loop (see PR33706 for more
  // details).
  if (PSE.getUnionPredicate().isAlwaysTrue()) {
    AllowedExit.insert(Phi);
    AllowedExit.insert(Phi->getIncomingValueForBlock(TheLoop->getLoopLatch()));
  }

  LLVM_DEBUG(dbgs() << "LV: Found an induction variable.\n");
}

bool LoopVectorizationLegality::setupOuterLoopInductions() {
  BasicBlock *Header = TheLoop->getHeader();

  // Returns true if a given Phi is a supported induction.
  auto isSupportedPhi = [&](PHINode &Phi) -> bool {
    InductionDescriptor ID;
    if (InductionDescriptor::isInductionPHI(&Phi, TheLoop, PSE, ID) &&
        ID.getKind() == InductionDescriptor::IK_IntInduction) {
      addInductionPhi(&Phi, ID, AllowedExit);
      return true;
    } else {
      // Bail out for any Phi in the outer loop header that is not a supported
      // induction.
      LLVM_DEBUG(
          dbgs()
          << "LV: Found unsupported PHI for outer loop vectorization.\n");
      return false;
    }
  };

  if (llvm::all_of(Header->phis(), isSupportedPhi))
    return true;
  else
    return false;
}

bool LoopVectorizationLegality::canVectorizeInstrs() {
  BasicBlock *Header = TheLoop->getHeader();

  // Look for the attribute signaling the absence of NaNs.
  Function &F = *Header->getParent();
  HasFunNoNaNAttr =
      F.getFnAttribute("no-nans-fp-math").getValueAsString() == "true";

  // For each block in the loop.
  for (BasicBlock *BB : TheLoop->blocks()) {
    // Scan the instructions in the block and look for hazards.
    for (Instruction &I : *BB) {
      if (auto *Phi = dyn_cast<PHINode>(&I)) {
        Type *PhiTy = Phi->getType();
        // Check that this PHI type is allowed.
        if (!PhiTy->isIntegerTy() && !PhiTy->isFloatingPointTy() &&
            !PhiTy->isPointerTy()) {
          reportVectorizationFailure("Found a non-int non-pointer PHI",
                                     "loop control flow is not understood by vectorizer",
                                     "CFGNotUnderstood");
          return false;
        }

        // If this PHINode is not in the header block, then we know that we
        // can convert it to select during if-conversion. No need to check if
        // the PHIs in this block are induction or reduction variables.
        if (BB != Header) {
          // Non-header phi nodes that have outside uses can be vectorized. Add
          // them to the list of allowed exits.
          // Unsafe cyclic dependencies with header phis are identified during
          // legalization for reduction, induction and first order
          // recurrences.
          continue;
        }

        // We only allow if-converted PHIs with exactly two incoming values.
        if (Phi->getNumIncomingValues() != 2) {
          reportVectorizationFailure("Found an invalid PHI",
              "loop control flow is not understood by vectorizer",
              "CFGNotUnderstood", Phi);
          return false;
        }

        RecurrenceDescriptor RedDes;
        if (RecurrenceDescriptor::isReductionPHI(Phi, TheLoop, RedDes, DB, AC,
                                                 DT)) {
          if (RedDes.hasUnsafeAlgebra())
            Requirements->addUnsafeAlgebraInst(RedDes.getUnsafeAlgebraInst());
          AllowedExit.insert(RedDes.getLoopExitInstr());
          Reductions[Phi] = RedDes;
          continue;
        }

        // TODO: Instead of recording the AllowedExit, it would be good to record the
        // complementary set: NotAllowedExit. These include (but may not be
        // limited to):
        // 1. Reduction phis as they represent the one-before-last value, which
        // is not available when vectorized 
        // 2. Induction phis and increment when SCEV predicates cannot be used
        // outside the loop - see addInductionPhi
        // 3. Non-Phis with outside uses when SCEV predicates cannot be used
        // outside the loop - see call to hasOutsideLoopUser in the non-phi
        // handling below
        // 4. FirstOrderRecurrence phis that can possibly be handled by
        // extraction.
        // By recording these, we can then reason about ways to vectorize each
        // of these NotAllowedExit. 
        InductionDescriptor ID;
        if (InductionDescriptor::isInductionPHI(Phi, TheLoop, PSE, ID)) {
          addInductionPhi(Phi, ID, AllowedExit);
          if (ID.hasUnsafeAlgebra() && !HasFunNoNaNAttr)
            Requirements->addUnsafeAlgebraInst(ID.getUnsafeAlgebraInst());
          continue;
        }

        if (RecurrenceDescriptor::isFirstOrderRecurrence(Phi, TheLoop,
                                                         SinkAfter, DT)) {
          FirstOrderRecurrences.insert(Phi);
          continue;
        }

        // As a last resort, coerce the PHI to a AddRec expression
        // and re-try classifying it a an induction PHI.
        if (InductionDescriptor::isInductionPHI(Phi, TheLoop, PSE, ID, true)) {
          addInductionPhi(Phi, ID, AllowedExit);
          continue;
        }

        reportVectorizationFailure("Found an unidentified PHI",
            "value that could not be identified as "
            "reduction is used outside the loop",
            "NonReductionValueUsedOutsideLoop", Phi);
        return false;
      } // end of PHI handling

      // We handle calls that:
      //   * Are debug info intrinsics.
      //   * Have a mapping to an IR intrinsic.
      //   * Have a vector version available.
      auto *CI = dyn_cast<CallInst>(&I);
      if (CI && !getVectorIntrinsicIDForCall(CI, TLI) &&
          !isa<DbgInfoIntrinsic>(CI) &&
          !(CI->getCalledFunction() && TLI &&
            TLI->isFunctionVectorizable(CI->getCalledFunction()->getName()))) {
        // If the call is a recognized math libary call, it is likely that
        // we can vectorize it given loosened floating-point constraints.
        LibFunc Func;
        bool IsMathLibCall =
            TLI && CI->getCalledFunction() &&
            CI->getType()->isFloatingPointTy() &&
            TLI->getLibFunc(CI->getCalledFunction()->getName(), Func) &&
            TLI->hasOptimizedCodeGen(Func);

        if (IsMathLibCall) {
          // TODO: Ideally, we should not use clang-specific language here,
          // but it's hard to provide meaningful yet generic advice.
          // Also, should this be guarded by allowExtraAnalysis() and/or be part
          // of the returned info from isFunctionVectorizable()?
          reportVectorizationFailure("Found a non-intrinsic callsite",
              "library call cannot be vectorized. "
              "Try compiling with -fno-math-errno, -ffast-math, "
              "or similar flags",
              "CantVectorizeLibcall", CI);
        } else {
          reportVectorizationFailure("Found a non-intrinsic callsite",
                                     "call instruction cannot be vectorized",
                                     "CantVectorizeLibcall", CI);
        }
        return false;
      }

      // Some intrinsics have scalar arguments and should be same in order for
      // them to be vectorized (i.e. loop invariant).
      if (CI) {
        auto *SE = PSE.getSE();
        Intrinsic::ID IntrinID = getVectorIntrinsicIDForCall(CI, TLI);
        for (unsigned i = 0, e = CI->getNumArgOperands(); i != e; ++i)
          if (hasVectorInstrinsicScalarOpd(IntrinID, i)) {
            if (!SE->isLoopInvariant(PSE.getSCEV(CI->getOperand(i)), TheLoop)) {
              reportVectorizationFailure("Found unvectorizable intrinsic",
                  "intrinsic instruction cannot be vectorized",
                  "CantVectorizeIntrinsic", CI);
              return false;
            }
          }
      }

      // Check that the instruction return type is vectorizable.
      // Also, we can't vectorize extractelement instructions.
      if ((!VectorType::isValidElementType(I.getType()) &&
           !I.getType()->isVoidTy()) ||
          isa<ExtractElementInst>(I)) {
        reportVectorizationFailure("Found unvectorizable type",
            "instruction return type cannot be vectorized",
            "CantVectorizeInstructionReturnType", &I);
        return false;
      }

      // Check that the stored type is vectorizable.
      if (auto *ST = dyn_cast<StoreInst>(&I)) {
        Type *T = ST->getValueOperand()->getType();
        if (!VectorType::isValidElementType(T)) {
          reportVectorizationFailure("Store instruction cannot be vectorized",
                                     "store instruction cannot be vectorized",
                                     "CantVectorizeStore", ST);
          return false;
        }

        // FP instructions can allow unsafe algebra, thus vectorizable by
        // non-IEEE-754 compliant SIMD units.
        // This applies to floating-point math operations and calls, not memory
        // operations, shuffles, or casts, as they don't change precision or
        // semantics.
      } else if (I.getType()->isFloatingPointTy() && (CI || I.isBinaryOp()) &&
                 !I.isFast()) {
        LLVM_DEBUG(dbgs() << "LV: Found FP op with unsafe algebra.\n");
        Hints->setPotentiallyUnsafe();
      }

      // Reduction instructions are allowed to have exit users.
      // All other instructions must not have external users.
      if (hasOutsideLoopUser(TheLoop, &I, AllowedExit)) {
        // We can safely vectorize loops where instructions within the loop are
        // used outside the loop only if the SCEV predicates within the loop is
        // same as outside the loop. Allowing the exit means reusing the SCEV
        // outside the loop.
        if (PSE.getUnionPredicate().isAlwaysTrue()) {
          AllowedExit.insert(&I);
          continue;
        }
        reportVectorizationFailure("Value cannot be used outside the loop",
                                   "value cannot be used outside the loop",
                                   "ValueUsedOutsideLoop", &I);
        return false;
      }
    } // next instr.
  }

  if (!PrimaryInduction) {
    if (Inductions.empty()) {
      reportVectorizationFailure("Did not find one integer induction var",
          "loop induction variable could not be identified",
          "NoInductionVariable");
      return false;
    } else if (!WidestIndTy) {
      reportVectorizationFailure("Did not find one integer induction var",
          "integer loop induction variable could not be identified",
          "NoIntegerInductionVariable");
      return false;
    } else {
      LLVM_DEBUG(dbgs() << "LV: Did not find one integer induction var.\n");
    }
  }

  // Now we know the widest induction type, check if our found induction
  // is the same size. If it's not, unset it here and InnerLoopVectorizer
  // will create another.
  if (PrimaryInduction && WidestIndTy != PrimaryInduction->getType())
    PrimaryInduction = nullptr;

  return true;
}

bool LoopVectorizationLegality::canVectorizeMemory() {
  LAI = &(*GetLAA)(*TheLoop);
  const OptimizationRemarkAnalysis *LAR = LAI->getReport();
  if (LAR) {
    ORE->emit([&]() {
      return OptimizationRemarkAnalysis(Hints->vectorizeAnalysisPassName(),
                                        "loop not vectorized: ", *LAR);
    });
  }
  if (!LAI->canVectorizeMemory())
    return false;

  if (LAI->hasDependenceInvolvingLoopInvariantAddress()) {
    reportVectorizationFailure("Stores to a uniform address",
        "write to a loop invariant address could not be vectorized",
        "CantVectorizeStoreToLoopInvariantAddress");
    return false;
  }
  Requirements->addRuntimePointerChecks(LAI->getNumRuntimePointerChecks());
  PSE.addPredicate(LAI->getPSE().getUnionPredicate());

  return true;
}

bool LoopVectorizationLegality::isInductionPhi(const Value *V) {
  Value *In0 = const_cast<Value *>(V);
  PHINode *PN = dyn_cast_or_null<PHINode>(In0);
  if (!PN)
    return false;

  return Inductions.count(PN);
}

bool LoopVectorizationLegality::isCastedInductionVariable(const Value *V) {
  auto *Inst = dyn_cast<Instruction>(V);
  return (Inst && InductionCastsToIgnore.count(Inst));
}

bool LoopVectorizationLegality::isInductionVariable(const Value *V) {
  return isInductionPhi(V) || isCastedInductionVariable(V);
}

bool LoopVectorizationLegality::isFirstOrderRecurrence(const PHINode *Phi) {
  return FirstOrderRecurrences.count(Phi);
}

bool LoopVectorizationLegality::blockNeedsPredication(BasicBlock *BB) {
  return LoopAccessInfo::blockNeedsPredication(BB, TheLoop, DT);
}

bool LoopVectorizationLegality::blockCanBePredicated(
    BasicBlock *BB, SmallPtrSetImpl<Value *> &SafePtrs) {
  const bool IsAnnotatedParallel = TheLoop->isAnnotatedParallel();

  for (Instruction &I : *BB) {
    // Check that we don't have a constant expression that can trap as operand.
    for (Value *Operand : I.operands()) {
      if (auto *C = dyn_cast<Constant>(Operand))
        if (C->canTrap())
          return false;
    }
    // We might be able to hoist the load.
    if (I.mayReadFromMemory()) {
      auto *LI = dyn_cast<LoadInst>(&I);
      if (!LI)
        return false;
      if (!SafePtrs.count(LI->getPointerOperand())) {
        // !llvm.mem.parallel_loop_access implies if-conversion safety.
        // Otherwise, record that the load needs (real or emulated) masking
        // and let the cost model decide.
        if (!IsAnnotatedParallel)
          MaskedOp.insert(LI);
        continue;
      }
    }

    if (I.mayWriteToMemory()) {
      auto *SI = dyn_cast<StoreInst>(&I);
      if (!SI)
        return false;
      // Predicated store requires some form of masking:
      // 1) masked store HW instruction,
      // 2) emulation via load-blend-store (only if safe and legal to do so,
      //    be aware on the race conditions), or
      // 3) element-by-element predicate check and scalar store.
      MaskedOp.insert(SI);
      continue;
    }
    if (I.mayThrow())
      return false;
  }

  return true;
}

bool LoopVectorizationLegality::canVectorizeWithIfConvert() {
  if (!EnableIfConversion) {
    reportVectorizationFailure("If-conversion is disabled",
                               "if-conversion is disabled",
                               "IfConversionDisabled");
    return false;
  }

  assert(TheLoop->getNumBlocks() > 1 && "Single block loops are vectorizable");

  // A list of pointers that we can safely read and write to.
  SmallPtrSet<Value *, 8> SafePointes;

  // Collect safe addresses.
  for (BasicBlock *BB : TheLoop->blocks()) {
    if (blockNeedsPredication(BB))
      continue;

    for (Instruction &I : *BB)
      if (auto *Ptr = getLoadStorePointerOperand(&I))
        SafePointes.insert(Ptr);
  }

  // Collect the blocks that need predication.
  BasicBlock *Header = TheLoop->getHeader();
  for (BasicBlock *BB : TheLoop->blocks()) {
    // We don't support switch statements inside loops.
    if (!isa<BranchInst>(BB->getTerminator())) {
      reportVectorizationFailure("Loop contains a switch statement",
                                 "loop contains a switch statement",
                                 "LoopContainsSwitch", BB->getTerminator());
      return false;
    }

    // We must be able to predicate all blocks that need to be predicated.
    if (blockNeedsPredication(BB)) {
      if (!blockCanBePredicated(BB, SafePointes)) {
        reportVectorizationFailure(
            "Control flow cannot be substituted for a select",
            "control flow cannot be substituted for a select",
            "NoCFGForSelect", BB->getTerminator());
        return false;
      }
    } else if (BB != Header && !canIfConvertPHINodes(BB)) {
      reportVectorizationFailure(
          "Control flow cannot be substituted for a select",
          "control flow cannot be substituted for a select",
          "NoCFGForSelect", BB->getTerminator());
      return false;
    }
  }

  // We can if-convert this loop.
  return true;
}

// Helper function to canVectorizeLoopNestCFG.
bool LoopVectorizationLegality::canVectorizeLoopCFG(Loop *Lp,
                                                    bool UseVPlanNativePath) {
  assert((UseVPlanNativePath || Lp->empty()) &&
         "VPlan-native path is not enabled.");

  // TODO: ORE should be improved to show more accurate information when an
  // outer loop can't be vectorized because a nested loop is not understood or
  // legal. Something like: "outer_loop_location: loop not vectorized:
  // (inner_loop_location) loop control flow is not understood by vectorizer".

  // Store the result and return it at the end instead of exiting early, in case
  // allowExtraAnalysis is used to report multiple reasons for not vectorizing.
  bool Result = true;
  bool DoExtraAnalysis = ORE->allowExtraAnalysis(DEBUG_TYPE);

  // We must have a loop in canonical form. Loops with indirectbr in them cannot
  // be canonicalized.
  if (!Lp->getLoopPreheader()) {
    reportVectorizationFailure("Loop doesn't have a legal pre-header",
        "loop control flow is not understood by vectorizer",
        "CFGNotUnderstood");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  // We must have a single backedge.
  if (Lp->getNumBackEdges() != 1) {
    reportVectorizationFailure("The loop must have a single backedge",
        "loop control flow is not understood by vectorizer",
        "CFGNotUnderstood");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  // We must have a single exiting block.
  if (!Lp->getExitingBlock()) {
    reportVectorizationFailure("The loop must have an exiting block",
        "loop control flow is not understood by vectorizer",
        "CFGNotUnderstood");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  // We only handle bottom-tested loops, i.e. loop in which the condition is
  // checked at the end of each iteration. With that we can assume that all
  // instructions in the loop are executed the same number of times.
  if (Lp->getExitingBlock() != Lp->getLoopLatch()) {
    reportVectorizationFailure("The exiting block is not the loop latch",
        "loop control flow is not understood by vectorizer",
        "CFGNotUnderstood");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  return Result;
}

bool LoopVectorizationLegality::canVectorizeLoopNestCFG(
    Loop *Lp, bool UseVPlanNativePath) {
  // Store the result and return it at the end instead of exiting early, in case
  // allowExtraAnalysis is used to report multiple reasons for not vectorizing.
  bool Result = true;
  bool DoExtraAnalysis = ORE->allowExtraAnalysis(DEBUG_TYPE);
  if (!canVectorizeLoopCFG(Lp, UseVPlanNativePath)) {
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  // Recursively check whether the loop control flow of nested loops is
  // understood.
  for (Loop *SubLp : *Lp)
    if (!canVectorizeLoopNestCFG(SubLp, UseVPlanNativePath)) {
      if (DoExtraAnalysis)
        Result = false;
      else
        return false;
    }

  return Result;
}

bool LoopVectorizationLegality::canVectorize(bool UseVPlanNativePath) {
  // Store the result and return it at the end instead of exiting early, in case
  // allowExtraAnalysis is used to report multiple reasons for not vectorizing.
  bool Result = true;

  bool DoExtraAnalysis = ORE->allowExtraAnalysis(DEBUG_TYPE);
  // Check whether the loop-related control flow in the loop nest is expected by
  // vectorizer.
  if (!canVectorizeLoopNestCFG(TheLoop, UseVPlanNativePath)) {
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  // We need to have a loop header.
  LLVM_DEBUG(dbgs() << "LV: Found a loop: " << TheLoop->getHeader()->getName()
                    << '\n');

  // Specific checks for outer loops. We skip the remaining legal checks at this
  // point because they don't support outer loops.
  if (!TheLoop->empty()) {
    assert(UseVPlanNativePath && "VPlan-native path is not enabled.");

    if (!canVectorizeOuterLoop()) {
      reportVectorizationFailure("Unsupported outer loop",
                                 "unsupported outer loop",
                                 "UnsupportedOuterLoop");
      // TODO: Implement DoExtraAnalysis when subsequent legal checks support
      // outer loops.
      return false;
    }

    LLVM_DEBUG(dbgs() << "LV: We can vectorize this outer loop!\n");
    return Result;
  }

  assert(TheLoop->empty() && "Inner loop expected.");
  // Check if we can if-convert non-single-bb loops.
  unsigned NumBlocks = TheLoop->getNumBlocks();
  if (NumBlocks != 1 && !canVectorizeWithIfConvert()) {
    LLVM_DEBUG(dbgs() << "LV: Can't if-convert the loop.\n");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  // Check if we can vectorize the instructions and CFG in this loop.
  if (!canVectorizeInstrs()) {
    LLVM_DEBUG(dbgs() << "LV: Can't vectorize the instructions or CFG\n");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  // Go over each instruction and look at memory deps.
  if (!canVectorizeMemory()) {
    LLVM_DEBUG(dbgs() << "LV: Can't vectorize due to memory conflicts\n");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  LLVM_DEBUG(dbgs() << "LV: We can vectorize this loop"
                    << (LAI->getRuntimePointerChecking()->Need
                            ? " (with a runtime bound check)"
                            : "")
                    << "!\n");

  unsigned SCEVThreshold = VectorizeSCEVCheckThreshold;
  if (Hints->getForce() == LoopVectorizeHints::FK_Enabled)
    SCEVThreshold = PragmaVectorizeSCEVCheckThreshold;

  if (PSE.getUnionPredicate().getComplexity() > SCEVThreshold) {
    reportVectorizationFailure("Too many SCEV checks needed",
        "Too many SCEV assumptions need to be made and checked at runtime",
        "TooManySCEVRunTimeChecks");
    if (DoExtraAnalysis)
      Result = false;
    else
      return false;
  }

  // Okay! We've done all the tests. If any have failed, return false. Otherwise
  // we can vectorize, and at this point we don't have any other mem analysis
  // which may limit our maximum vectorization factor, so just return true with
  // no restrictions.
  return Result;
}

bool LoopVectorizationLegality::canFoldTailByMasking() {

  LLVM_DEBUG(dbgs() << "LV: checking if tail can be folded by masking.\n");

  if (!PrimaryInduction) {
    reportVectorizationFailure(
        "No primary induction, cannot fold tail by masking",
        "Missing a primary induction variable in the loop, which is "
        "needed in order to fold tail by masking as required.",
        "NoPrimaryInduction");
    return false;
  }

  // TODO: handle reductions when tail is folded by masking.
  if (!Reductions.empty()) {
    reportVectorizationFailure(
        "Loop has reductions, cannot fold tail by masking",
        "Cannot fold tail by masking in the presence of reductions.",
        "ReductionFoldingTailByMasking");
    return false;
  }

  // TODO: handle outside users when tail is folded by masking.
  for (auto *AE : AllowedExit) {
    // Check that all users of allowed exit values are inside the loop.
    for (User *U : AE->users()) {
      Instruction *UI = cast<Instruction>(U);
      if (TheLoop->contains(UI))
        continue;
      reportVectorizationFailure(
          "Cannot fold tail by masking, loop has an outside user for",
          "Cannot fold tail by masking in the presence of live outs.",
          "LiveOutFoldingTailByMasking", UI);
      return false;
    }
  }

  // The list of pointers that we can safely read and write to remains empty.
  SmallPtrSet<Value *, 8> SafePointers;

  // Check and mark all blocks for predication, including those that ordinarily
  // do not need predication such as the header block.
  for (BasicBlock *BB : TheLoop->blocks()) {
    if (!blockCanBePredicated(BB, SafePointers)) {
      reportVectorizationFailure(
          "Cannot fold tail by masking as required",
          "control flow cannot be substituted for a select",
          "NoCFGForSelect", BB->getTerminator());
      return false;
    }
  }

  LLVM_DEBUG(dbgs() << "LV: can fold tail by masking.\n");
  return true;
}

} // namespace llvm