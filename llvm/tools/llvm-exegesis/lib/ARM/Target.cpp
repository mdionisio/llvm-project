//===-- Target.cpp ----------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// The ARM exegesis target. Only Thumb-2 code is supported, and the remote
// execution mode assumes an M-profile core (ARMv7-M / ARMv8-M Mainline).
//
//===----------------------------------------------------------------------===//

#include "../Target.h"
#include "ARM.h"
#include "ARMBaseRegisterInfo.h"
#include "MCTargetDesc/ARMBaseInfo.h"
#include "MCTargetDesc/ARMMCTargetDesc.h"
#include "Utils/ARMBaseInfo.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/MC/MCInstBuilder.h"

#define GET_AVAILABLE_OPCODE_CHECKER
#include "ARMGenInstrInfo.inc"

namespace llvm {
namespace exegesis {

#include "ARMGenExegesis.inc"

// R11 is the loop counter. Being unavailable to the snippets, it is also the
// temporary of the register setup: the loop counter is set after it.
constexpr MCPhysReg kDefaultLoopCounterReg = ARM::R11;
constexpr MCPhysReg kSetupTmpReg = kDefaultLoopCounterReg;

// Temporaries of the remote counter reads: they run before the register
// setup and after the snippet, so the snippet never sees them. Neither is the
// argument register (R0), SP or LR.
constexpr MCPhysReg kCounterAddrReg = ARM::R3;
constexpr MCPhysReg kCounterValueReg = ARM::R12;

// "msr apsr_nzcvq, rN" on M-profile.
constexpr unsigned kAPSRNZCVQMask = 0x800;

// Debug Exception and Monitor Control Register and Data Watchpoint and Trace
// unit of ARMv7-M / ARMv8-M.
constexpr uint32_t kDEMCR = 0xE000EDFC;
constexpr uint32_t kDEMCRTraceEnable = 1u << 24;
constexpr uint32_t kDWTCtrl = 0xE0001000;
constexpr uint32_t kDWTCtrlCycCntEnable = 1u;
constexpr uint32_t kDWTCycCnt = 0xE0001004;
// Coprocessor Access Control Register: full access to CP10 and CP11 (FPU).
constexpr uint32_t kCPACR = 0xE000ED88;
constexpr uint32_t kCPACRFullFPAccess = 0xFu << 20;

// Appends the "always" predicate.
static MCInst withPred(MCInstBuilder B) {
  return B.addImm(ARMCC::AL).addReg(0);
}

// movw (and movt if needed) Reg, Value.
static void loadImmediate32(std::vector<MCInst> &Insts, MCRegister Reg,
                            uint32_t Value) {
  Insts.push_back(withPred(
      MCInstBuilder(ARM::t2MOVi16).addReg(Reg).addImm(Value & 0xffff)));
  if (Value >> 16)
    Insts.push_back(withPred(MCInstBuilder(ARM::t2MOVTi16)
                                 .addReg(Reg)
                                 .addReg(Reg)
                                 .addImm(Value >> 16)));
}

static MCInst loadWord(MCRegister Rt, MCRegister Rn, unsigned Offset = 0) {
  return withPred(
      MCInstBuilder(ARM::t2LDRi12).addReg(Rt).addReg(Rn).addImm(Offset));
}

static MCInst storeWord(MCRegister Rt, MCRegister Rn, unsigned Offset = 0) {
  return withPred(
      MCInstBuilder(ARM::t2STRi12).addReg(Rt).addReg(Rn).addImm(Offset));
}

// Sets Bits in the 32-bit word at Address.
static void orrWord(std::vector<MCInst> &Insts, uint32_t Address,
                    uint32_t Bits) {
  loadImmediate32(Insts, kCounterAddrReg, Address);
  Insts.push_back(loadWord(kCounterValueReg, kCounterAddrReg));
  Insts.push_back(MCInstBuilder(ARM::t2ORRri)
                      .addReg(kCounterValueReg)
                      .addReg(kCounterValueReg)
                      .addImm(Bits)
                      .addImm(ARMCC::AL)
                      .addReg(0)
                      .addReg(0)); // no flags
  Insts.push_back(storeWord(kCounterValueReg, kCounterAddrReg));
}

// Stores the 32-bit value at CounterAddress in the low word of the 8-byte
// slot at Address, and zero in the high word. The barriers make sure that
// the instructions before the read have completed, which matters on dual
// issue cores such as Cortex-M7.
static void readCounterAt(std::vector<MCInst> &Insts, uint64_t Address,
                          uint32_t CounterAddress) {
  loadImmediate32(Insts, kCounterAddrReg, CounterAddress);
  Insts.push_back(withPred(MCInstBuilder(ARM::t2DSB).addImm(ARM_MB::SY)));
  Insts.push_back(withPred(MCInstBuilder(ARM::t2ISB).addImm(ARM_ISB::SY)));
  Insts.push_back(loadWord(kCounterValueReg, kCounterAddrReg));
  loadImmediate32(Insts, kCounterAddrReg, Address);
  Insts.push_back(storeWord(kCounterValueReg, kCounterAddrReg));
  loadImmediate32(Insts, kCounterValueReg, 0);
  Insts.push_back(storeWord(kCounterValueReg, kCounterAddrReg, 4));
}

// Like AArch64 does with CPACR_EL1, give the snippets access to the FPU
// (a no-op on cores without one). The barriers of the counter read that
// follows make it visible to the snippet.
static void enableFPU(std::vector<MCInst> &Insts) {
  orrWord(Insts, kCPACR, kCPACRFullFPAccess);
}

namespace {

class ExegesisARMTarget : public ExegesisTarget {
public:
  ExegesisARMTarget()
      : ExegesisTarget(ARMCpuPfmCounters, ARM_MC::isOpcodeAvailable) {}

private:
  std::vector<MCInst> setRegTo(const MCSubtargetInfo &STI, MCRegister Reg,
                               const APInt &Value) const override {
    std::vector<MCInst> Insts;
    if (ARM::rGPRRegClass.contains(Reg)) {
      loadImmediate32(Insts, Reg, Value.getZExtValue());
      return Insts;
    }
    if (ARM::SPRRegClass.contains(Reg)) {
      loadImmediate32(Insts, kSetupTmpReg, Value.getZExtValue());
      Insts.push_back(withPred(
          MCInstBuilder(ARM::VMOVSR).addReg(Reg).addReg(kSetupTmpReg)));
      return Insts;
    }
    if (Reg >= ARM::D0 && Reg <= ARM::D15) {
      // Through the two aliasing S registers.
      const MCRegister Low = ARM::S0 + 2 * (Reg - ARM::D0);
      const APInt Value64 = Value.zextOrTrunc(64);
      for (unsigned Half = 0; Half < 2; ++Half) {
        loadImmediate32(Insts, kSetupTmpReg,
                        Value64.extractBitsAsZExtValue(32, 32 * Half));
        Insts.push_back(withPred(MCInstBuilder(ARM::VMOVSR)
                                     .addReg(Low + Half)
                                     .addReg(kSetupTmpReg)));
      }
      return Insts;
    }
    if (Reg == ARM::CPSR) {
      // The flags are the top bits of APSR.
      loadImmediate32(Insts, kSetupTmpReg, Value.getZExtValue());
      Insts.push_back(withPred(MCInstBuilder(ARM::t2MSR_M)
                                   .addImm(kAPSRNZCVQMask)
                                   .addReg(kSetupTmpReg)));
      return Insts;
    }
    if (Reg == ARM::FPSCR || Reg == ARM::FPSCR_RM || Reg == ARM::FPSCR_NZCV) {
      // The whole FPSCR, with the value in the field of the register.
      const unsigned Shift =
          Reg == ARM::FPSCR_RM ? 22 : (Reg == ARM::FPSCR_NZCV ? 28 : 0);
      loadImmediate32(Insts, kSetupTmpReg, Value.getZExtValue() << Shift);
      Insts.push_back(withPred(MCInstBuilder(ARM::VMSR).addReg(kSetupTmpReg)));
      return Insts;
    }
    errs() << "setRegTo is not implemented, results will be unreliable\n";
    return {};
  }

  Error
  randomizeTargetMCOperand(const Instruction &Instr, const Variable &Var,
                           MCOperand &AssignedValue,
                           const BitVector &ForbiddenRegs) const override {
    const MCOperandInfo &OpInfo =
        Instr.getPrimaryOperand(Var).getExplicitOperandInfo();
    // Unconditional execution: the AL condition code and no CPSR operand.
    if (OpInfo.isPredicate()) {
      AssignedValue = OpInfo.RegClass < 0 ? MCOperand::createImm(ARMCC::AL)
                                          : MCOperand::createReg(0);
      return Error::success();
    }
    // Leave the flags alone: the non flag-setting form.
    if (OpInfo.isOptionalDef()) {
      AssignedValue = MCOperand::createReg(0);
      return Error::success();
    }
    return make_error<Failure>(
        Twine("unimplemented operand type ").concat(Twine(OpInfo.OperandType)));
  }

  MCRegister getDefaultLoopCounterRegister(const Triple &) const override {
    return kDefaultLoopCounterReg;
  }

  void decrementLoopCounterAndJump(MachineBasicBlock &MBB,
                                   MachineBasicBlock &TargetMBB,
                                   const MCInstrInfo &MII,
                                   MCRegister LoopRegister) const override {
    // subs.w LoopRegister, LoopRegister, #1
    BuildMI(&MBB, DebugLoc(), MII.get(ARM::t2SUBri))
        .addDef(LoopRegister)
        .addUse(LoopRegister)
        .addImm(1)
        .addImm(ARMCC::AL)
        .addReg(0)
        .addReg(ARM::CPSR, RegState::Define);
    // bne TargetMBB
    BuildMI(&MBB, DebugLoc(), MII.get(ARM::t2Bcc))
        .addMBB(&TargetMBB)
        .addImm(ARMCC::NE)
        .addReg(ARM::CPSR);
  }

  // Registers that should not be selected for use in snippets.
  const MCPhysReg UnavailableRegisters[1] = {kDefaultLoopCounterReg};
  ArrayRef<MCPhysReg> getUnavailableRegisters() const override {
    return UnavailableRegisters;
  }

  bool matchesArch(Triple::ArchType Arch) const override {
    return Arch == Triple::thumb || Arch == Triple::thumbeb;
  }

  std::optional<RemoteABI> getRemoteABI() const override {
    // M-profile: "b ." is 0xe7fe. The return address in LR has bit 0 set,
    // otherwise `bx lr` leaves Thumb state and faults, and xPSR.T must be set
    // when the debugger starts the core at the benchmark function.
    return RemoteABI{"r0",
                     "sp",
                     "lr",
                     32,
                     {0xfe, 0xe7},
                     /*LinkRegisterTag=*/1,
                     {{"xpsr", 0x01000000}}};
  }

  // DWT CYCCNT. The first read also enables it (DEMCR.TRCENA and
  // DWT_CTRL.CYCCNTENA): cheap enough to repeat before each run, and it does
  // not depend on the debugger being able to write the System Control Space
  // (QEMU ignores such writes).
  std::vector<MCInst> readRemoteCycleCounter(uint64_t Address,
                                             bool Start) const override {
    std::vector<MCInst> Insts;
    if (Start) {
      enableFPU(Insts);
      orrWord(Insts, kDEMCR, kDEMCRTraceEnable);
      orrWord(Insts, kDWTCtrl, kDWTCtrlCycCntEnable);
    }
    readCounterAt(Insts, Address, kDWTCycCnt);
    return Insts;
  }

  std::vector<MCInst> readRemoteMemoryCounter(
      uint64_t Address, bool Start, uint64_t CounterAddress,
      ArrayRef<std::pair<uint64_t, uint32_t>> Setup) const override {
    std::vector<MCInst> Insts;
    if (Start) {
      enableFPU(Insts);
      for (auto [Addr, Value] : Setup) {
        loadImmediate32(Insts, kCounterAddrReg, Addr);
        loadImmediate32(Insts, kCounterValueReg, Value);
        Insts.push_back(storeWord(kCounterValueReg, kCounterAddrReg));
      }
    }
    readCounterAt(Insts, Address, CounterAddress);
    return Insts;
  }
};

} // namespace

static ExegesisTarget *getTheExegesisARMTarget() {
  static ExegesisARMTarget Target;
  return &Target;
}

void InitializeARMExegesisTarget() {
  ExegesisTarget::registerTarget(getTheExegesisARMTarget());
}

} // namespace exegesis
} // namespace llvm
