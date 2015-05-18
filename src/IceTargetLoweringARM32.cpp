//===- subzero/src/IceTargetLoweringARM32.cpp - ARM32 lowering ------------===//
//
//                        The Subzero Code Generator
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the TargetLoweringARM32 class, which consists almost
// entirely of the lowering sequence for each high-level instruction.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MathExtras.h"

#include "IceCfg.h"
#include "IceCfgNode.h"
#include "IceClFlags.h"
#include "IceDefs.h"
#include "IceELFObjectWriter.h"
#include "IceGlobalInits.h"
#include "IceInstARM32.h"
#include "IceLiveness.h"
#include "IceOperand.h"
#include "IceRegistersARM32.h"
#include "IceTargetLoweringARM32.def"
#include "IceTargetLoweringARM32.h"
#include "IceUtils.h"

namespace Ice {

namespace {
void UnimplementedError(const ClFlags &Flags) {
  if (!Flags.getSkipUnimplemented()) {
    // Use llvm_unreachable instead of report_fatal_error, which gives better
    // stack traces.
    llvm_unreachable("Not yet implemented");
    abort();
  }
}

// The maximum number of arguments to pass in GPR registers.
const uint32_t ARM32_MAX_GPR_ARG = 4;

} // end of anonymous namespace

TargetARM32::TargetARM32(Cfg *Func)
    : TargetLowering(Func), UsesFramePointer(false) {
  // TODO: Don't initialize IntegerRegisters and friends every time.
  // Instead, initialize in some sort of static initializer for the
  // class.
  llvm::SmallBitVector IntegerRegisters(RegARM32::Reg_NUM);
  llvm::SmallBitVector FloatRegisters(RegARM32::Reg_NUM);
  llvm::SmallBitVector VectorRegisters(RegARM32::Reg_NUM);
  llvm::SmallBitVector InvalidRegisters(RegARM32::Reg_NUM);
  ScratchRegs.resize(RegARM32::Reg_NUM);
#define X(val, encode, name, scratch, preserved, stackptr, frameptr, isInt,    \
          isFP)                                                                \
  IntegerRegisters[RegARM32::val] = isInt;                                     \
  FloatRegisters[RegARM32::val] = isFP;                                        \
  VectorRegisters[RegARM32::val] = isFP;                                       \
  ScratchRegs[RegARM32::val] = scratch;
  REGARM32_TABLE;
#undef X
  TypeToRegisterSet[IceType_void] = InvalidRegisters;
  TypeToRegisterSet[IceType_i1] = IntegerRegisters;
  TypeToRegisterSet[IceType_i8] = IntegerRegisters;
  TypeToRegisterSet[IceType_i16] = IntegerRegisters;
  TypeToRegisterSet[IceType_i32] = IntegerRegisters;
  TypeToRegisterSet[IceType_i64] = IntegerRegisters;
  TypeToRegisterSet[IceType_f32] = FloatRegisters;
  TypeToRegisterSet[IceType_f64] = FloatRegisters;
  TypeToRegisterSet[IceType_v4i1] = VectorRegisters;
  TypeToRegisterSet[IceType_v8i1] = VectorRegisters;
  TypeToRegisterSet[IceType_v16i1] = VectorRegisters;
  TypeToRegisterSet[IceType_v16i8] = VectorRegisters;
  TypeToRegisterSet[IceType_v8i16] = VectorRegisters;
  TypeToRegisterSet[IceType_v4i32] = VectorRegisters;
  TypeToRegisterSet[IceType_v4f32] = VectorRegisters;
}

void TargetARM32::translateO2() {
  TimerMarker T(TimerStack::TT_O2, Func);

  // TODO(stichnot): share passes with X86?
  // https://code.google.com/p/nativeclient/issues/detail?id=4094

  if (!Ctx->getFlags().getPhiEdgeSplit()) {
    // Lower Phi instructions.
    Func->placePhiLoads();
    if (Func->hasError())
      return;
    Func->placePhiStores();
    if (Func->hasError())
      return;
    Func->deletePhis();
    if (Func->hasError())
      return;
    Func->dump("After Phi lowering");
  }

  // Address mode optimization.
  Func->getVMetadata()->init(VMK_SingleDefs);
  Func->doAddressOpt();

  // Argument lowering
  Func->doArgLowering();

  // Target lowering.  This requires liveness analysis for some parts
  // of the lowering decisions, such as compare/branch fusing.  If
  // non-lightweight liveness analysis is used, the instructions need
  // to be renumbered first.  TODO: This renumbering should only be
  // necessary if we're actually calculating live intervals, which we
  // only do for register allocation.
  Func->renumberInstructions();
  if (Func->hasError())
    return;

  // TODO: It should be sufficient to use the fastest liveness
  // calculation, i.e. livenessLightweight().  However, for some
  // reason that slows down the rest of the translation.  Investigate.
  Func->liveness(Liveness_Basic);
  if (Func->hasError())
    return;
  Func->dump("After ARM32 address mode opt");

  Func->genCode();
  if (Func->hasError())
    return;
  Func->dump("After ARM32 codegen");

  // Register allocation.  This requires instruction renumbering and
  // full liveness analysis.
  Func->renumberInstructions();
  if (Func->hasError())
    return;
  Func->liveness(Liveness_Intervals);
  if (Func->hasError())
    return;
  // Validate the live range computations.  The expensive validation
  // call is deliberately only made when assertions are enabled.
  assert(Func->validateLiveness());
  // The post-codegen dump is done here, after liveness analysis and
  // associated cleanup, to make the dump cleaner and more useful.
  Func->dump("After initial ARM32 codegen");
  Func->getVMetadata()->init(VMK_All);
  regAlloc(RAK_Global);
  if (Func->hasError())
    return;
  Func->dump("After linear scan regalloc");

  if (Ctx->getFlags().getPhiEdgeSplit()) {
    Func->advancedPhiLowering();
    Func->dump("After advanced Phi lowering");
  }

  // Stack frame mapping.
  Func->genFrame();
  if (Func->hasError())
    return;
  Func->dump("After stack frame mapping");

  Func->contractEmptyNodes();
  Func->reorderNodes();

  // Branch optimization.  This needs to be done just before code
  // emission.  In particular, no transformations that insert or
  // reorder CfgNodes should be done after branch optimization.  We go
  // ahead and do it before nop insertion to reduce the amount of work
  // needed for searching for opportunities.
  Func->doBranchOpt();
  Func->dump("After branch optimization");

  // Nop insertion
  if (Ctx->getFlags().shouldDoNopInsertion()) {
    Func->doNopInsertion();
  }
}

void TargetARM32::translateOm1() {
  TimerMarker T(TimerStack::TT_Om1, Func);

  // TODO: share passes with X86?

  Func->placePhiLoads();
  if (Func->hasError())
    return;
  Func->placePhiStores();
  if (Func->hasError())
    return;
  Func->deletePhis();
  if (Func->hasError())
    return;
  Func->dump("After Phi lowering");

  Func->doArgLowering();

  Func->genCode();
  if (Func->hasError())
    return;
  Func->dump("After initial ARM32 codegen");

  regAlloc(RAK_InfOnly);
  if (Func->hasError())
    return;
  Func->dump("After regalloc of infinite-weight variables");

  Func->genFrame();
  if (Func->hasError())
    return;
  Func->dump("After stack frame mapping");

  // Nop insertion
  if (Ctx->getFlags().shouldDoNopInsertion()) {
    Func->doNopInsertion();
  }
}

bool TargetARM32::doBranchOpt(Inst *I, const CfgNode *NextNode) {
  (void)I;
  (void)NextNode;
  UnimplementedError(Func->getContext()->getFlags());
  return false;
}

IceString TargetARM32::RegNames[] = {
#define X(val, encode, name, scratch, preserved, stackptr, frameptr, isInt,    \
          isFP)                                                                \
  name,
    REGARM32_TABLE
#undef X
};

IceString TargetARM32::getRegName(SizeT RegNum, Type Ty) const {
  assert(RegNum < RegARM32::Reg_NUM);
  (void)Ty;
  return RegNames[RegNum];
}

Variable *TargetARM32::getPhysicalRegister(SizeT RegNum, Type Ty) {
  if (Ty == IceType_void)
    Ty = IceType_i32;
  if (PhysicalRegisters[Ty].empty())
    PhysicalRegisters[Ty].resize(RegARM32::Reg_NUM);
  assert(RegNum < PhysicalRegisters[Ty].size());
  Variable *Reg = PhysicalRegisters[Ty][RegNum];
  if (Reg == nullptr) {
    Reg = Func->makeVariable(Ty);
    Reg->setRegNum(RegNum);
    PhysicalRegisters[Ty][RegNum] = Reg;
    // Specially mark SP and LR as an "argument" so that it is considered
    // live upon function entry.
    if (RegNum == RegARM32::Reg_sp || RegNum == RegARM32::Reg_lr) {
      Func->addImplicitArg(Reg);
      Reg->setIgnoreLiveness();
    }
  }
  return Reg;
}

void TargetARM32::emitVariable(const Variable *Var) const {
  Ostream &Str = Ctx->getStrEmit();
  if (Var->hasReg()) {
    Str << getRegName(Var->getRegNum(), Var->getType());
    return;
  }
  if (Var->getWeight().isInf()) {
    llvm::report_fatal_error(
        "Infinite-weight Variable has no register assigned");
  }
  int32_t Offset = Var->getStackOffset();
  if (!hasFramePointer())
    Offset += getStackAdjustment();
  // TODO(jvoung): Handle out of range. Perhaps we need a scratch register
  // to materialize a larger offset.
  const bool SignExt = false;
  if (!OperandARM32Mem::canHoldOffset(Var->getType(), SignExt, Offset)) {
    llvm::report_fatal_error("Illegal stack offset");
  }
  const Type FrameSPTy = IceType_i32;
  Str << "[" << getRegName(getFrameOrStackReg(), FrameSPTy);
  if (Offset != 0) {
    Str << ", " << getConstantPrefix() << Offset;
  }
  Str << "]";
}

void TargetARM32::lowerArguments() {
  VarList &Args = Func->getArgs();
  // The first few integer type parameters can use r0-r3, regardless of their
  // position relative to the floating-point/vector arguments in the argument
  // list. Floating-point and vector arguments can use q0-q3 (aka d0-d7,
  // s0-s15).
  unsigned NumGPRRegsUsed = 0;

  // For each register argument, replace Arg in the argument list with the
  // home register.  Then generate an instruction in the prolog to copy the
  // home register to the assigned location of Arg.
  Context.init(Func->getEntryNode());
  Context.setInsertPoint(Context.getCur());

  for (SizeT I = 0, E = Args.size(); I < E; ++I) {
    Variable *Arg = Args[I];
    Type Ty = Arg->getType();
    // TODO(jvoung): handle float/vector types.
    if (isVectorType(Ty)) {
      UnimplementedError(Func->getContext()->getFlags());
      continue;
    } else if (isFloatingType(Ty)) {
      UnimplementedError(Func->getContext()->getFlags());
      continue;
    } else if (Ty == IceType_i64) {
      if (NumGPRRegsUsed >= ARM32_MAX_GPR_ARG)
        continue;
      int32_t RegLo = RegARM32::Reg_r0 + NumGPRRegsUsed;
      int32_t RegHi = 0;
      ++NumGPRRegsUsed;
      // Always start i64 registers at an even register, so this may end
      // up padding away a register.
      if (RegLo % 2 != 0) {
        ++RegLo;
        ++NumGPRRegsUsed;
      }
      // If this leaves us without room to consume another register,
      // leave any previously speculatively consumed registers as consumed.
      if (NumGPRRegsUsed >= ARM32_MAX_GPR_ARG)
        continue;
      RegHi = RegARM32::Reg_r0 + NumGPRRegsUsed;
      ++NumGPRRegsUsed;
      Variable *RegisterArg = Func->makeVariable(Ty);
      Variable *RegisterLo = Func->makeVariable(IceType_i32);
      Variable *RegisterHi = Func->makeVariable(IceType_i32);
      if (ALLOW_DUMP) {
        RegisterArg->setName(Func, "home_reg:" + Arg->getName(Func));
        RegisterLo->setName(Func, "home_reg_lo:" + Arg->getName(Func));
        RegisterHi->setName(Func, "home_reg_hi:" + Arg->getName(Func));
      }
      RegisterLo->setRegNum(RegLo);
      RegisterLo->setIsArg();
      RegisterHi->setRegNum(RegHi);
      RegisterHi->setIsArg();
      RegisterArg->setLoHi(RegisterLo, RegisterHi);
      RegisterArg->setIsArg();
      Arg->setIsArg(false);

      Args[I] = RegisterArg;
      Context.insert(InstAssign::create(Func, Arg, RegisterArg));
      continue;
    } else {
      assert(Ty == IceType_i32);
      if (NumGPRRegsUsed >= ARM32_MAX_GPR_ARG)
        continue;
      int32_t RegNum = RegARM32::Reg_r0 + NumGPRRegsUsed;
      ++NumGPRRegsUsed;
      Variable *RegisterArg = Func->makeVariable(Ty);
      if (ALLOW_DUMP) {
        RegisterArg->setName(Func, "home_reg:" + Arg->getName(Func));
      }
      RegisterArg->setRegNum(RegNum);
      RegisterArg->setIsArg();
      Arg->setIsArg(false);

      Args[I] = RegisterArg;
      Context.insert(InstAssign::create(Func, Arg, RegisterArg));
    }
  }
}

Type TargetARM32::stackSlotType() { return IceType_i32; }

void TargetARM32::addProlog(CfgNode *Node) {
  (void)Node;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::addEpilog(CfgNode *Node) {
  (void)Node;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::split64(Variable *Var) {
  assert(Var->getType() == IceType_i64);
  Variable *Lo = Var->getLo();
  Variable *Hi = Var->getHi();
  if (Lo) {
    assert(Hi);
    return;
  }
  assert(Hi == nullptr);
  Lo = Func->makeVariable(IceType_i32);
  Hi = Func->makeVariable(IceType_i32);
  if (ALLOW_DUMP) {
    Lo->setName(Func, Var->getName(Func) + "__lo");
    Hi->setName(Func, Var->getName(Func) + "__hi");
  }
  Var->setLoHi(Lo, Hi);
  if (Var->getIsArg()) {
    Lo->setIsArg();
    Hi->setIsArg();
  }
}

Operand *TargetARM32::loOperand(Operand *Operand) {
  assert(Operand->getType() == IceType_i64);
  if (Operand->getType() != IceType_i64)
    return Operand;
  if (Variable *Var = llvm::dyn_cast<Variable>(Operand)) {
    split64(Var);
    return Var->getLo();
  }
  if (ConstantInteger64 *Const = llvm::dyn_cast<ConstantInteger64>(Operand)) {
    return Ctx->getConstantInt32(static_cast<uint32_t>(Const->getValue()));
  }
  if (OperandARM32Mem *Mem = llvm::dyn_cast<OperandARM32Mem>(Operand)) {
    // Conservatively disallow memory operands with side-effects (pre/post
    // increment) in case of duplication.
    assert(Mem->getAddrMode() == OperandARM32Mem::Offset ||
           Mem->getAddrMode() == OperandARM32Mem::NegOffset);
    if (Mem->isRegReg()) {
      return OperandARM32Mem::create(Func, IceType_i32, Mem->getBase(),
                                     Mem->getIndex(), Mem->getShiftOp(),
                                     Mem->getShiftAmt(), Mem->getAddrMode());
    } else {
      return OperandARM32Mem::create(Func, IceType_i32, Mem->getBase(),
                                     Mem->getOffset(), Mem->getAddrMode());
    }
  }
  llvm_unreachable("Unsupported operand type");
  return nullptr;
}

Operand *TargetARM32::hiOperand(Operand *Operand) {
  assert(Operand->getType() == IceType_i64);
  if (Operand->getType() != IceType_i64)
    return Operand;
  if (Variable *Var = llvm::dyn_cast<Variable>(Operand)) {
    split64(Var);
    return Var->getHi();
  }
  if (ConstantInteger64 *Const = llvm::dyn_cast<ConstantInteger64>(Operand)) {
    return Ctx->getConstantInt32(
        static_cast<uint32_t>(Const->getValue() >> 32));
  }
  if (OperandARM32Mem *Mem = llvm::dyn_cast<OperandARM32Mem>(Operand)) {
    // Conservatively disallow memory operands with side-effects
    // in case of duplication.
    assert(Mem->getAddrMode() == OperandARM32Mem::Offset ||
           Mem->getAddrMode() == OperandARM32Mem::NegOffset);
    const Type SplitType = IceType_i32;
    if (Mem->isRegReg()) {
      // We have to make a temp variable T, and add 4 to either Base or Index.
      // The Index may be shifted, so adding 4 can mean something else.
      // Thus, prefer T := Base + 4, and use T as the new Base.
      Variable *Base = Mem->getBase();
      Constant *Four = Ctx->getConstantInt32(4);
      Variable *NewBase = Func->makeVariable(Base->getType());
      lowerArithmetic(InstArithmetic::create(Func, InstArithmetic::Add, NewBase,
                                             Base, Four));
      return OperandARM32Mem::create(Func, SplitType, NewBase, Mem->getIndex(),
                                     Mem->getShiftOp(), Mem->getShiftAmt(),
                                     Mem->getAddrMode());
    } else {
      Variable *Base = Mem->getBase();
      ConstantInteger32 *Offset = Mem->getOffset();
      assert(!Utils::WouldOverflowAdd(Offset->getValue(), 4));
      int32_t NextOffsetVal = Offset->getValue() + 4;
      const bool SignExt = false;
      if (!OperandARM32Mem::canHoldOffset(SplitType, SignExt, NextOffsetVal)) {
        // We have to make a temp variable and add 4 to either Base or Offset.
        // If we add 4 to Offset, this will convert a non-RegReg addressing
        // mode into a RegReg addressing mode. Since NaCl sandboxing disallows
        // RegReg addressing modes, prefer adding to base and replacing instead.
        // Thus we leave the old offset alone.
        Constant *Four = Ctx->getConstantInt32(4);
        Variable *NewBase = Func->makeVariable(Base->getType());
        lowerArithmetic(InstArithmetic::create(Func, InstArithmetic::Add,
                                               NewBase, Base, Four));
        Base = NewBase;
      } else {
        Offset =
            llvm::cast<ConstantInteger32>(Ctx->getConstantInt32(NextOffsetVal));
      }
      return OperandARM32Mem::create(Func, SplitType, Base, Offset,
                                     Mem->getAddrMode());
    }
  }
  llvm_unreachable("Unsupported operand type");
  return nullptr;
}

llvm::SmallBitVector TargetARM32::getRegisterSet(RegSetMask Include,
                                                 RegSetMask Exclude) const {
  llvm::SmallBitVector Registers(RegARM32::Reg_NUM);

#define X(val, encode, name, scratch, preserved, stackptr, frameptr, isInt,    \
          isFP)                                                                \
  if (scratch && (Include & RegSet_CallerSave))                                \
    Registers[RegARM32::val] = true;                                           \
  if (preserved && (Include & RegSet_CalleeSave))                              \
    Registers[RegARM32::val] = true;                                           \
  if (stackptr && (Include & RegSet_StackPointer))                             \
    Registers[RegARM32::val] = true;                                           \
  if (frameptr && (Include & RegSet_FramePointer))                             \
    Registers[RegARM32::val] = true;                                           \
  if (scratch && (Exclude & RegSet_CallerSave))                                \
    Registers[RegARM32::val] = false;                                          \
  if (preserved && (Exclude & RegSet_CalleeSave))                              \
    Registers[RegARM32::val] = false;                                          \
  if (stackptr && (Exclude & RegSet_StackPointer))                             \
    Registers[RegARM32::val] = false;                                          \
  if (frameptr && (Exclude & RegSet_FramePointer))                             \
    Registers[RegARM32::val] = false;

  REGARM32_TABLE

#undef X

  return Registers;
}

void TargetARM32::lowerAlloca(const InstAlloca *Inst) {
  UsesFramePointer = true;
  // Conservatively require the stack to be aligned.  Some stack
  // adjustment operations implemented below assume that the stack is
  // aligned before the alloca.  All the alloca code ensures that the
  // stack alignment is preserved after the alloca.  The stack alignment
  // restriction can be relaxed in some cases.
  NeedsStackAlignment = true;
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerArithmetic(const InstArithmetic *Inst) {
  Variable *Dest = Inst->getDest();
  // TODO(jvoung): Should be able to flip Src0 and Src1 if it is easier
  // to legalize Src0 to flex or Src1 to flex and there is a reversible
  // instruction. E.g., reverse subtract with immediate, register vs
  // register, immediate.
  // Or it may be the case that the operands aren't swapped, but the
  // bits can be flipped and a different operation applied.
  // E.g., use BIC (bit clear) instead of AND for some masks.
  Variable *Src0 = legalizeToVar(Inst->getSrc(0));
  Operand *Src1 = legalize(Inst->getSrc(1), Legal_Reg | Legal_Flex);
  (void)Src0;
  (void)Src1;
  if (Dest->getType() == IceType_i64) {
    UnimplementedError(Func->getContext()->getFlags());
  } else if (isVectorType(Dest->getType())) {
    UnimplementedError(Func->getContext()->getFlags());
  } else { // Dest->getType() is non-i64 scalar
    switch (Inst->getOp()) {
    case InstArithmetic::_num:
      llvm_unreachable("Unknown arithmetic operator");
      break;
    case InstArithmetic::Add: {
      UnimplementedError(Func->getContext()->getFlags());
      // Variable *T = makeReg(Dest->getType());
      // _add(T, Src0, Src1);
      // _mov(Dest, T);
    } break;
    case InstArithmetic::And:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Or:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Xor:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Sub:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Mul:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Shl:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Lshr:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Ashr:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Udiv:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Sdiv:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Urem:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Srem:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Fadd:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Fsub:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Fmul:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Fdiv:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    case InstArithmetic::Frem:
      UnimplementedError(Func->getContext()->getFlags());
      break;
    }
  }
}

void TargetARM32::lowerAssign(const InstAssign *Inst) {
  Variable *Dest = Inst->getDest();
  Operand *Src0 = Inst->getSrc(0);
  assert(Dest->getType() == Src0->getType());
  if (Dest->getType() == IceType_i64) {
    Src0 = legalize(Src0);
    Operand *Src0Lo = loOperand(Src0);
    Operand *Src0Hi = hiOperand(Src0);
    Variable *DestLo = llvm::cast<Variable>(loOperand(Dest));
    Variable *DestHi = llvm::cast<Variable>(hiOperand(Dest));
    Variable *T_Lo = nullptr, *T_Hi = nullptr;
    _mov(T_Lo, Src0Lo);
    _mov(DestLo, T_Lo);
    _mov(T_Hi, Src0Hi);
    _mov(DestHi, T_Hi);
  } else {
    Operand *SrcR;
    if (Dest->hasReg()) {
      // If Dest already has a physical register, then legalize the
      // Src operand into a Variable with the same register
      // assignment.  This is mostly a workaround for advanced phi
      // lowering's ad-hoc register allocation which assumes no
      // register allocation is needed when at least one of the
      // operands is non-memory.
      // TODO(jvoung): check this for ARM.
      SrcR = legalize(Src0, Legal_Reg, Dest->getRegNum());
    } else {
      // Dest could be a stack operand. Since we could potentially need
      // to do a Store (and store can only have Register operands),
      // legalize this to a register.
      SrcR = legalize(Src0, Legal_Reg);
    }
    if (isVectorType(Dest->getType())) {
      UnimplementedError(Func->getContext()->getFlags());
    } else {
      _mov(Dest, SrcR);
    }
  }
}

void TargetARM32::lowerBr(const InstBr *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerCall(const InstCall *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerCast(const InstCast *Inst) {
  InstCast::OpKind CastKind = Inst->getCastKind();
  switch (CastKind) {
  default:
    Func->setError("Cast type not supported");
    return;
  case InstCast::Sext: {
    UnimplementedError(Func->getContext()->getFlags());
    break;
  }
  case InstCast::Zext: {
    UnimplementedError(Func->getContext()->getFlags());
    break;
  }
  case InstCast::Trunc: {
    UnimplementedError(Func->getContext()->getFlags());
    break;
  }
  case InstCast::Fptrunc:
    UnimplementedError(Func->getContext()->getFlags());
    break;
  case InstCast::Fpext: {
    UnimplementedError(Func->getContext()->getFlags());
    break;
  }
  case InstCast::Fptosi:
    UnimplementedError(Func->getContext()->getFlags());
    break;
  case InstCast::Fptoui:
    UnimplementedError(Func->getContext()->getFlags());
    break;
  case InstCast::Sitofp:
    UnimplementedError(Func->getContext()->getFlags());
    break;
  case InstCast::Uitofp: {
    UnimplementedError(Func->getContext()->getFlags());
    break;
  }
  case InstCast::Bitcast: {
    UnimplementedError(Func->getContext()->getFlags());
    break;
  }
  }
}

void TargetARM32::lowerExtractElement(const InstExtractElement *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerFcmp(const InstFcmp *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerIcmp(const InstIcmp *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerInsertElement(const InstInsertElement *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerIntrinsicCall(const InstIntrinsicCall *Instr) {
  switch (Intrinsics::IntrinsicID ID = Instr->getIntrinsicInfo().ID) {
  case Intrinsics::AtomicCmpxchg: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::AtomicFence:
    UnimplementedError(Func->getContext()->getFlags());
    return;
  case Intrinsics::AtomicFenceAll:
    // NOTE: FenceAll should prevent and load/store from being moved
    // across the fence (both atomic and non-atomic). The InstARM32Mfence
    // instruction is currently marked coarsely as "HasSideEffects".
    UnimplementedError(Func->getContext()->getFlags());
    return;
  case Intrinsics::AtomicIsLockFree: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::AtomicLoad: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::AtomicRMW:
    UnimplementedError(Func->getContext()->getFlags());
    return;
  case Intrinsics::AtomicStore: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::Bswap: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::Ctpop: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::Ctlz: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::Cttz: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::Fabs: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::Longjmp: {
    InstCall *Call = makeHelperCall(H_call_longjmp, nullptr, 2);
    Call->addArg(Instr->getArg(0));
    Call->addArg(Instr->getArg(1));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Memcpy: {
    // In the future, we could potentially emit an inline memcpy/memset, etc.
    // for intrinsic calls w/ a known length.
    InstCall *Call = makeHelperCall(H_call_memcpy, nullptr, 3);
    Call->addArg(Instr->getArg(0));
    Call->addArg(Instr->getArg(1));
    Call->addArg(Instr->getArg(2));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Memmove: {
    InstCall *Call = makeHelperCall(H_call_memmove, nullptr, 3);
    Call->addArg(Instr->getArg(0));
    Call->addArg(Instr->getArg(1));
    Call->addArg(Instr->getArg(2));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Memset: {
    // The value operand needs to be extended to a stack slot size
    // because the PNaCl ABI requires arguments to be at least 32 bits
    // wide.
    Operand *ValOp = Instr->getArg(1);
    assert(ValOp->getType() == IceType_i8);
    Variable *ValExt = Func->makeVariable(stackSlotType());
    lowerCast(InstCast::create(Func, InstCast::Zext, ValExt, ValOp));
    InstCall *Call = makeHelperCall(H_call_memset, nullptr, 3);
    Call->addArg(Instr->getArg(0));
    Call->addArg(ValExt);
    Call->addArg(Instr->getArg(2));
    lowerCall(Call);
    return;
  }
  case Intrinsics::NaClReadTP: {
    if (Ctx->getFlags().getUseSandboxing()) {
      UnimplementedError(Func->getContext()->getFlags());
    } else {
      InstCall *Call = makeHelperCall(H_call_read_tp, Instr->getDest(), 0);
      lowerCall(Call);
    }
    return;
  }
  case Intrinsics::Setjmp: {
    InstCall *Call = makeHelperCall(H_call_setjmp, Instr->getDest(), 1);
    Call->addArg(Instr->getArg(0));
    lowerCall(Call);
    return;
  }
  case Intrinsics::Sqrt: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::Stacksave: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::Stackrestore: {
    UnimplementedError(Func->getContext()->getFlags());
    return;
  }
  case Intrinsics::Trap:
    UnimplementedError(Func->getContext()->getFlags());
    return;
  case Intrinsics::UnknownIntrinsic:
    Func->setError("Should not be lowering UnknownIntrinsic");
    return;
  }
  return;
}

void TargetARM32::lowerLoad(const InstLoad *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::doAddressOptLoad() {
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::randomlyInsertNop(float Probability) {
  RandomNumberGeneratorWrapper RNG(Ctx->getRNG());
  if (RNG.getTrueWithProbability(Probability)) {
    UnimplementedError(Func->getContext()->getFlags());
  }
}

void TargetARM32::lowerPhi(const InstPhi * /*Inst*/) {
  Func->setError("Phi found in regular instruction list");
}

void TargetARM32::lowerRet(const InstRet *Inst) {
  Variable *Reg = nullptr;
  if (Inst->hasRetValue()) {
    Operand *Src0 = Inst->getRetValue();
    if (Src0->getType() == IceType_i64) {
      Variable *R0 = legalizeToVar(loOperand(Src0), RegARM32::Reg_r0);
      Variable *R1 = legalizeToVar(hiOperand(Src0), RegARM32::Reg_r1);
      Reg = R0;
      Context.insert(InstFakeUse::create(Func, R1));
    } else if (isScalarFloatingType(Src0->getType())) {
      UnimplementedError(Func->getContext()->getFlags());
    } else if (isVectorType(Src0->getType())) {
      UnimplementedError(Func->getContext()->getFlags());
    } else {
      Operand *Src0F = legalize(Src0, Legal_Reg | Legal_Flex);
      _mov(Reg, Src0F, RegARM32::Reg_r0);
    }
  }
  // Add a ret instruction even if sandboxing is enabled, because
  // addEpilog explicitly looks for a ret instruction as a marker for
  // where to insert the frame removal instructions.
  // addEpilog is responsible for restoring the "lr" register as needed
  // prior to this ret instruction.
  _ret(getPhysicalRegister(RegARM32::Reg_lr), Reg);
  // Add a fake use of sp to make sure sp stays alive for the entire
  // function.  Otherwise post-call sp adjustments get dead-code
  // eliminated.  TODO: Are there more places where the fake use
  // should be inserted?  E.g. "void f(int n){while(1) g(n);}" may not
  // have a ret instruction.
  Variable *SP = Func->getTarget()->getPhysicalRegister(RegARM32::Reg_sp);
  Context.insert(InstFakeUse::create(Func, SP));
}

void TargetARM32::lowerSelect(const InstSelect *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerStore(const InstStore *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::doAddressOptStore() {
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerSwitch(const InstSwitch *Inst) {
  (void)Inst;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::lowerUnreachable(const InstUnreachable * /*Inst*/) {
  UnimplementedError(Func->getContext()->getFlags());
}

// Turn an i64 Phi instruction into a pair of i32 Phi instructions, to
// preserve integrity of liveness analysis.  Undef values are also
// turned into zeroes, since loOperand() and hiOperand() don't expect
// Undef input.
void TargetARM32::prelowerPhis() {
  UnimplementedError(Func->getContext()->getFlags());
}

// Lower the pre-ordered list of assignments into mov instructions.
// Also has to do some ad-hoc register allocation as necessary.
void TargetARM32::lowerPhiAssignments(CfgNode *Node,
                                      const AssignList &Assignments) {
  (void)Node;
  (void)Assignments;
  UnimplementedError(Func->getContext()->getFlags());
}

Variable *TargetARM32::makeVectorOfZeros(Type Ty, int32_t RegNum) {
  Variable *Reg = makeReg(Ty, RegNum);
  UnimplementedError(Func->getContext()->getFlags());
  return Reg;
}

// Helper for legalize() to emit the right code to lower an operand to a
// register of the appropriate type.
Variable *TargetARM32::copyToReg(Operand *Src, int32_t RegNum) {
  Type Ty = Src->getType();
  Variable *Reg = makeReg(Ty, RegNum);
  if (isVectorType(Ty)) {
    UnimplementedError(Func->getContext()->getFlags());
  } else {
    // Mov's Src operand can really only be the flexible second operand type
    // or a register. Users should guarantee that.
    _mov(Reg, Src);
  }
  return Reg;
}

Operand *TargetARM32::legalize(Operand *From, LegalMask Allowed,
                               int32_t RegNum) {
  // Assert that a physical register is allowed.  To date, all calls
  // to legalize() allow a physical register. Legal_Flex converts
  // registers to the right type OperandARM32FlexReg as needed.
  assert(Allowed & Legal_Reg);
  // Go through the various types of operands:
  // OperandARM32Mem, OperandARM32Flex, Constant, and Variable.
  // Given the above assertion, if type of operand is not legal
  // (e.g., OperandARM32Mem and !Legal_Mem), we can always copy
  // to a register.
  if (auto Mem = llvm::dyn_cast<OperandARM32Mem>(From)) {
    // Before doing anything with a Mem operand, we need to ensure
    // that the Base and Index components are in physical registers.
    Variable *Base = Mem->getBase();
    Variable *Index = Mem->getIndex();
    Variable *RegBase = nullptr;
    Variable *RegIndex = nullptr;
    if (Base) {
      RegBase = legalizeToVar(Base);
    }
    if (Index) {
      RegIndex = legalizeToVar(Index);
    }
    // Create a new operand if there was a change.
    if (Base != RegBase || Index != RegIndex) {
      // There is only a reg +/- reg or reg + imm form.
      // Figure out which to re-create.
      if (Mem->isRegReg()) {
        Mem = OperandARM32Mem::create(Func, Mem->getType(), RegBase, RegIndex,
                                      Mem->getShiftOp(), Mem->getShiftAmt(),
                                      Mem->getAddrMode());
      } else {
        Mem = OperandARM32Mem::create(Func, Mem->getType(), RegBase,
                                      Mem->getOffset(), Mem->getAddrMode());
      }
    }
    if (!(Allowed & Legal_Mem)) {
      Type Ty = Mem->getType();
      Variable *Reg = makeReg(Ty, RegNum);
      _ldr(Reg, Mem);
      From = Reg;
    } else {
      From = Mem;
    }
    return From;
  }

  if (auto Flex = llvm::dyn_cast<OperandARM32Flex>(From)) {
    if (!(Allowed & Legal_Flex)) {
      if (auto FlexReg = llvm::dyn_cast<OperandARM32FlexReg>(Flex)) {
        if (FlexReg->getShiftOp() == OperandARM32::kNoShift) {
          From = FlexReg->getReg();
          // Fall through and let From be checked as a Variable below,
          // where it may or may not need a register.
        } else {
          return copyToReg(Flex, RegNum);
        }
      } else {
        return copyToReg(Flex, RegNum);
      }
    } else {
      return From;
    }
  }

  if (llvm::isa<Constant>(From)) {
    if (llvm::isa<ConstantUndef>(From)) {
      // Lower undefs to zero.  Another option is to lower undefs to an
      // uninitialized register; however, using an uninitialized register
      // results in less predictable code.
      if (isVectorType(From->getType()))
        return makeVectorOfZeros(From->getType(), RegNum);
      From = Ctx->getConstantZero(From->getType());
    }
    // There should be no constants of vector type (other than undef).
    assert(!isVectorType(From->getType()));
    bool CanBeFlex = Allowed & Legal_Flex;
    if (auto C32 = llvm::dyn_cast<ConstantInteger32>(From)) {
      uint32_t RotateAmt;
      uint32_t Immed_8;
      uint32_t Value = static_cast<uint32_t>(C32->getValue());
      // Check if the immediate will fit in a Flexible second operand,
      // if a Flexible second operand is allowed. We need to know the exact
      // value, so that rules out relocatable constants.
      // Also try the inverse and use MVN if possible.
      if (CanBeFlex &&
          OperandARM32FlexImm::canHoldImm(Value, &RotateAmt, &Immed_8)) {
        return OperandARM32FlexImm::create(Func, From->getType(), Immed_8,
                                           RotateAmt);
      } else if (CanBeFlex && OperandARM32FlexImm::canHoldImm(
                                  ~Value, &RotateAmt, &Immed_8)) {
        auto InvertedFlex = OperandARM32FlexImm::create(Func, From->getType(),
                                                        Immed_8, RotateAmt);
        Type Ty = From->getType();
        Variable *Reg = makeReg(Ty, RegNum);
        _mvn(Reg, InvertedFlex);
        return Reg;
      } else {
        // Do a movw/movt to a register.
        Type Ty = From->getType();
        Variable *Reg = makeReg(Ty, RegNum);
        uint32_t UpperBits = (Value >> 16) & 0xFFFF;
        _movw(Reg,
              UpperBits != 0 ? Ctx->getConstantInt32(Value & 0xFFFF) : C32);
        if (UpperBits != 0) {
          _movt(Reg, Ctx->getConstantInt32(UpperBits));
        }
        return Reg;
      }
    } else if (auto C = llvm::dyn_cast<ConstantRelocatable>(From)) {
      Type Ty = From->getType();
      Variable *Reg = makeReg(Ty, RegNum);
      _movw(Reg, C);
      _movt(Reg, C);
      return Reg;
    } else {
      // Load floats/doubles from literal pool.
      UnimplementedError(Func->getContext()->getFlags());
      From = copyToReg(From, RegNum);
    }
    return From;
  }

  if (auto Var = llvm::dyn_cast<Variable>(From)) {
    // Check if the variable is guaranteed a physical register.  This
    // can happen either when the variable is pre-colored or when it is
    // assigned infinite weight.
    bool MustHaveRegister = (Var->hasReg() || Var->getWeight().isInf());
    // We need a new physical register for the operand if:
    //   Mem is not allowed and Var isn't guaranteed a physical
    //   register, or
    //   RegNum is required and Var->getRegNum() doesn't match.
    if ((!(Allowed & Legal_Mem) && !MustHaveRegister) ||
        (RegNum != Variable::NoRegister && RegNum != Var->getRegNum())) {
      From = copyToReg(From, RegNum);
    }
    return From;
  }
  llvm_unreachable("Unhandled operand kind in legalize()");

  return From;
}

// Provide a trivial wrapper to legalize() for this common usage.
Variable *TargetARM32::legalizeToVar(Operand *From, int32_t RegNum) {
  return llvm::cast<Variable>(legalize(From, Legal_Reg, RegNum));
}

Variable *TargetARM32::makeReg(Type Type, int32_t RegNum) {
  // There aren't any 64-bit integer registers for ARM32.
  assert(Type != IceType_i64);
  Variable *Reg = Func->makeVariable(Type);
  if (RegNum == Variable::NoRegister)
    Reg->setWeightInfinite();
  else
    Reg->setRegNum(RegNum);
  return Reg;
}

void TargetARM32::postLower() {
  if (Ctx->getFlags().getOptLevel() == Opt_m1)
    return;
  inferTwoAddress();
}

void TargetARM32::makeRandomRegisterPermutation(
    llvm::SmallVectorImpl<int32_t> &Permutation,
    const llvm::SmallBitVector &ExcludeRegisters) const {
  (void)Permutation;
  (void)ExcludeRegisters;
  UnimplementedError(Func->getContext()->getFlags());
}

void TargetARM32::emit(const ConstantInteger32 *C) const {
  if (!ALLOW_DUMP)
    return;
  Ostream &Str = Ctx->getStrEmit();
  Str << getConstantPrefix() << C->getValue();
}

void TargetARM32::emit(const ConstantInteger64 *) const {
  llvm::report_fatal_error("Not expecting to emit 64-bit integers");
}

void TargetARM32::emit(const ConstantFloat *C) const {
  (void)C;
  UnimplementedError(Ctx->getFlags());
}

void TargetARM32::emit(const ConstantDouble *C) const {
  (void)C;
  UnimplementedError(Ctx->getFlags());
}

void TargetARM32::emit(const ConstantUndef *) const {
  llvm::report_fatal_error("undef value encountered by emitter.");
}

TargetDataARM32::TargetDataARM32(GlobalContext *Ctx)
    : TargetDataLowering(Ctx) {}

void TargetDataARM32::lowerGlobal(const VariableDeclaration &Var) const {
  (void)Var;
  UnimplementedError(Ctx->getFlags());
}

void TargetDataARM32::lowerGlobals(
    std::unique_ptr<VariableDeclarationList> Vars) const {
  switch (Ctx->getFlags().getOutFileType()) {
  case FT_Elf: {
    ELFObjectWriter *Writer = Ctx->getObjectWriter();
    Writer->writeDataSection(*Vars, llvm::ELF::R_ARM_ABS32);
  } break;
  case FT_Asm:
  case FT_Iasm: {
    const IceString &TranslateOnly = Ctx->getFlags().getTranslateOnly();
    OstreamLocker L(Ctx);
    for (const VariableDeclaration *Var : *Vars) {
      if (GlobalContext::matchSymbolName(Var->getName(), TranslateOnly)) {
        lowerGlobal(*Var);
      }
    }
  } break;
  }
}

void TargetDataARM32::lowerConstants() const {
  if (Ctx->getFlags().getDisableTranslation())
    return;
  UnimplementedError(Ctx->getFlags());
}

} // end of namespace Ice
