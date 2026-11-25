//===-- RemoteExecution.h ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execution of benchmarks on a remote, bare-metal core driven by a debugger.
///
/// llvm-exegesis talks to a persistent helper process on its stdin/stdout
/// with a line protocol (version 1):
///
///   hello 1                           -> ok 1 <name>
///   write <addr> <hex>                -> ok
///   read <addr> <len>                 -> ok <hex>
///   run <pc> <stop> <timeout_ms> <reg>=<val>...
///                                     -> ok halted <pc>
///   cmd <text>                        -> ok <hex output>
///   quit                              -> ok
///
/// Any failure is answered with `err <message>`. The helper owns the
/// transport (GDB remote protocol, a probe SDK, ...); the target description
/// says where code, scratch memory, stack and results live.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_EXEGESIS_REMOTEEXECUTION_H
#define LLVM_TOOLS_LLVM_EXEGESIS_REMOTEEXECUTION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
namespace exegesis {

class ExegesisTarget;

/// The memory layout and setup of a remote target, read from YAML.
struct RemoteTargetDescription {
  struct Region {
    uint64_t Base = 0;
    uint64_t Size = 0;
  };
  /// One initialization step: either a raw debugger command or a 32-bit
  /// memory write.
  struct InitStep {
    std::optional<std::string> Monitor;
    std::vector<uint64_t> Write32; // {address, value}
  };

  /// A memory-mapped counter that replaces the default cycle counter of the
  /// target, e.g. a timer of the SoC or SysTick.
  struct MemoryCounter {
    uint64_t Address = 0;
    unsigned Width = 32;     // bits that count, for the wrap-around
    bool CountsDown = false; // the elapsed count is start - end
    uint64_t Scale = 1;      // core cycles per count
    /// 32-bit (address, value) stores done by the generated code before the
    /// first read of each run, e.g. to enable the counter.
    std::vector<std::pair<uint64_t, uint32_t>> Setup;
  };

  unsigned Version = 0;
  Region Code;
  Region Scratch;
  uint64_t StackTop = 0;
  uint64_t Results = 0;
  unsigned TimeoutMs = 2000;
  std::vector<InitStep> Init;
  /// Steps run after a failed run (fault or timeout), e.g. a reset of the
  /// core: an M-profile core left in a fault handler locks up on the next
  /// fault.
  std::vector<InitStep> Recover;
  std::optional<MemoryCounter> Counter;

  /// Size of the results area: start and end counter values, 8 bytes each.
  static constexpr uint64_t kResultsSize = 16;
  /// Offset of the benchmark function in the code region. The return pad
  /// sits at the start of the region.
  static constexpr uint64_t kFunctionOffset = 0x1000;

  uint64_t returnPadAddress() const { return Code.Base; }
  uint64_t functionAddress() const { return Code.Base + kFunctionOffset; }

  static Expected<RemoteTargetDescription> parse(StringRef Yaml);
  static Expected<RemoteTargetDescription> readFile(StringRef Path);
  /// Checks the fields and that the regions do not overlap.
  Error validate() const;
};

/// A running helper process.
class RemoteHelper {
public:
  static Expected<std::unique_ptr<RemoteHelper>> launch(StringRef Command);
  ~RemoteHelper();

  /// Sends one request and returns the payload after "ok".
  Expected<std::string> request(StringRef Line);

  Error writeMemory(uint64_t Address, ArrayRef<uint8_t> Data);
  Expected<std::vector<uint8_t>> readMemory(uint64_t Address, uint64_t Size);
  /// Sets the registers, runs from PC and waits for the core to stop at Stop.
  /// Returns the PC the core halted at.
  Expected<uint64_t> run(uint64_t PC, uint64_t Stop, unsigned TimeoutMs,
                         ArrayRef<std::pair<std::string, uint64_t>> Registers);
  Expected<std::string> command(StringRef Text);

private:
  RemoteHelper(int Pid, int ToHelper, int FromHelper)
      : Pid(Pid), ToHelper(ToHelper), FromHelper(FromHelper) {}
  Expected<std::string> readLine();

  int Pid;
  int ToHelper;
  int FromHelper;
  std::string Pending; // bytes read past the last newline
};

/// A helper plus a target description, ready to run benchmark functions.
class RemoteSession {
public:
  static Expected<std::unique_ptr<RemoteSession>>
  create(StringRef HelperCommand, StringRef DescriptionPath,
         const ExegesisTarget &ET);
  /// A session without helper: enough to assemble the benchmark functions
  /// (e.g. --benchmark-phase=assemble-measured-code), not to run them.
  static Expected<std::unique_ptr<RemoteSession>>
  createOffline(StringRef DescriptionPath, const ExegesisTarget &ET);

  const RemoteTargetDescription &getDescription() const { return Desc; }

  /// Loads the benchmark function contained in Obj, runs it once and returns
  /// the difference of the two counter values it stored.
  Expected<int64_t> runOnce(const object::ObjectFile &Obj) const;

private:
  RemoteSession(RemoteTargetDescription Desc,
                std::unique_ptr<RemoteHelper> Helper, const ExegesisTarget &ET)
      : Desc(std::move(Desc)), Helper(std::move(Helper)), ET(ET) {}
  Error initialize();
  Error runSteps(ArrayRef<RemoteTargetDescription::InitStep> Steps) const;

  RemoteTargetDescription Desc;
  std::unique_ptr<RemoteHelper> Helper;
  const ExegesisTarget &ET;
};

} // namespace exegesis
} // namespace llvm

#endif // LLVM_TOOLS_LLVM_EXEGESIS_REMOTEEXECUTION_H
