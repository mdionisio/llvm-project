//===-- RemoteExecution.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RemoteExecution.h"
#include "Assembler.h"
#include "Error.h"
#include "Target.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"

#ifdef LLVM_ON_UNIX
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace llvm {
namespace exegesis {

//===----------------------------------------------------------------------===//
// Target description
//===----------------------------------------------------------------------===//

namespace {
// YAML mirror of RemoteTargetDescription with hexadecimal fields.
struct YamlRegion {
  yaml::Hex64 Base = 0;
  yaml::Hex64 Size = 0;
};
struct YamlStack {
  yaml::Hex64 Top = 0;
};
struct YamlInitStep {
  std::optional<std::string> Monitor;
  std::vector<yaml::Hex64> Write32;
};
struct YamlCounter {
  yaml::Hex64 Address = 0;
  unsigned Width = 32;
  bool Down = false;
  uint64_t Scale = 1;
  std::vector<YamlInitStep> Setup;
};
struct YamlDescription {
  unsigned Version = 0;
  YamlRegion Code;
  YamlRegion Scratch;
  YamlStack Stack;
  yaml::Hex64 Results = 0;
  unsigned TimeoutMs = 2000;
  std::vector<YamlInitStep> Init;
  std::vector<YamlInitStep> Recover;
  std::optional<YamlCounter> Counter;
};
} // namespace
} // namespace exegesis
} // namespace llvm

LLVM_YAML_IS_SEQUENCE_VECTOR(llvm::exegesis::YamlInitStep)

namespace llvm {
namespace yaml {
template <> struct MappingTraits<exegesis::YamlRegion> {
  static void mapping(IO &Io, exegesis::YamlRegion &R) {
    Io.mapRequired("base", R.Base);
    Io.mapRequired("size", R.Size);
  }
};
template <> struct MappingTraits<exegesis::YamlStack> {
  static void mapping(IO &Io, exegesis::YamlStack &S) {
    Io.mapRequired("top", S.Top);
  }
};
template <> struct MappingTraits<exegesis::YamlInitStep> {
  static void mapping(IO &Io, exegesis::YamlInitStep &S) {
    Io.mapOptional("monitor", S.Monitor);
    Io.mapOptional("write32", S.Write32);
  }
  static std::string validate(IO &, exegesis::YamlInitStep &S) {
    const bool HasWrite = !S.Write32.empty();
    if (S.Monitor.has_value() == HasWrite)
      return "an init step needs exactly one of 'monitor' or 'write32'";
    if (HasWrite && S.Write32.size() != 2)
      return "'write32' needs [address, value]";
    return "";
  }
};
template <> struct MappingTraits<exegesis::YamlCounter> {
  static void mapping(IO &Io, exegesis::YamlCounter &C) {
    Io.mapRequired("address", C.Address);
    Io.mapOptional("width", C.Width, 32u);
    Io.mapOptional("down", C.Down, false);
    Io.mapOptional("scale", C.Scale, uint64_t(1));
    Io.mapOptional("setup", C.Setup);
  }
  static std::string validate(IO &, exegesis::YamlCounter &C) {
    if (C.Width == 0 || C.Width > 32)
      return "the counter 'width' must be between 1 and 32";
    if (C.Scale == 0)
      return "the counter 'scale' must not be 0";
    for (const exegesis::YamlInitStep &S : C.Setup)
      if (S.Monitor)
        return "the counter 'setup' only takes 'write32' steps";
    return "";
  }
};
template <> struct MappingTraits<exegesis::YamlDescription> {
  static void mapping(IO &Io, exegesis::YamlDescription &D) {
    Io.mapRequired("version", D.Version);
    Io.mapRequired("code", D.Code);
    Io.mapRequired("scratch", D.Scratch);
    Io.mapRequired("stack", D.Stack);
    Io.mapRequired("results", D.Results);
    Io.mapOptional("timeout_ms", D.TimeoutMs, 2000u);
    Io.mapOptional("init", D.Init);
    Io.mapOptional("recover", D.Recover);
    Io.mapOptional("counter", D.Counter);
  }
};
} // namespace yaml
} // namespace llvm

namespace llvm {
namespace exegesis {

Expected<RemoteTargetDescription>
RemoteTargetDescription::parse(StringRef Yaml) {
  YamlDescription YD;
  std::string Diagnostics;
  {
    yaml::Input In(
        Yaml, nullptr,
        [](const SMDiagnostic &Diag, void *Ctx) {
          raw_string_ostream OS(*static_cast<std::string *>(Ctx));
          Diag.print("remote target", OS, /*ShowColors=*/false);
        },
        &Diagnostics);
    In >> YD;
    if (In.error())
      return make_error<Failure>("invalid remote target description:\n" +
                                 Diagnostics);
  }
  RemoteTargetDescription D;
  D.Version = YD.Version;
  D.Code = {YD.Code.Base, YD.Code.Size};
  D.Scratch = {YD.Scratch.Base, YD.Scratch.Size};
  D.StackTop = YD.Stack.Top;
  D.Results = YD.Results;
  D.TimeoutMs = YD.TimeoutMs;
  for (auto [From, To] :
       {std::pair{&YD.Init, &D.Init}, std::pair{&YD.Recover, &D.Recover}}) {
    for (const YamlInitStep &S : *From) {
      InitStep Step;
      Step.Monitor = S.Monitor;
      for (yaml::Hex64 V : S.Write32)
        Step.Write32.push_back(V);
      To->push_back(std::move(Step));
    }
  }
  if (YD.Counter) {
    RemoteTargetDescription::MemoryCounter C;
    C.Address = YD.Counter->Address;
    C.Width = YD.Counter->Width;
    C.CountsDown = YD.Counter->Down;
    C.Scale = YD.Counter->Scale;
    for (const YamlInitStep &S : YD.Counter->Setup)
      C.Setup.emplace_back(S.Write32[0], static_cast<uint32_t>(S.Write32[1]));
    D.Counter = std::move(C);
  }
  if (Error E = D.validate())
    return std::move(E);
  return D;
}

Expected<RemoteTargetDescription>
RemoteTargetDescription::readFile(StringRef Path) {
  auto Buffer = MemoryBuffer::getFile(Path, /*IsText=*/true);
  if (!Buffer)
    return make_error<Failure>("cannot read remote target description '" +
                               Path + "': " + Buffer.getError().message());
  return parse((*Buffer)->getBuffer());
}

Error RemoteTargetDescription::validate() const {
  if (Version != 1)
    return make_error<Failure>(
        "unsupported remote target description version " + Twine(Version) +
        " (expected 1)");
  if (Code.Size <= kFunctionOffset)
    return make_error<Failure>("the code region must be larger than " +
                               Twine::utohexstr(kFunctionOffset) + " bytes");
  if (Scratch.Size == 0)
    return make_error<Failure>("the scratch region is empty");
  struct Named {
    const char *Name;
    uint64_t Begin, End;
  };
  const Named Regions[] = {
      {"code", Code.Base, Code.Base + Code.Size},
      {"scratch", Scratch.Base, Scratch.Base + Scratch.Size},
      {"results", Results, Results + kResultsSize},
  };
  for (unsigned I = 0; I < std::size(Regions); ++I)
    for (unsigned J = I + 1; J < std::size(Regions); ++J)
      if (Regions[I].Begin < Regions[J].End &&
          Regions[J].Begin < Regions[I].End)
        return make_error<Failure>(Twine("the '") + Regions[I].Name +
                                   "' and '" + Regions[J].Name +
                                   "' regions overlap");
  for (const Named &R : Regions)
    if (StackTop > R.Begin && StackTop <= R.End)
      return make_error<Failure>(Twine("the stack top lies in the '") + R.Name +
                                 "' region");
  return Error::success();
}

//===----------------------------------------------------------------------===//
// Helper process
//===----------------------------------------------------------------------===//

#ifdef LLVM_ON_UNIX
Expected<std::unique_ptr<RemoteHelper>>
RemoteHelper::launch(StringRef Command) {
  int ToChild[2], FromChild[2];
  if (pipe(ToChild) != 0 || pipe(FromChild) != 0)
    return make_error<Failure>("cannot create the pipes for the remote helper");
  std::string Cmd = Command.str();
  const pid_t Pid = fork();
  if (Pid < 0)
    return make_error<Failure>("cannot fork the remote helper");
  if (Pid == 0) {
    dup2(ToChild[0], STDIN_FILENO);
    dup2(FromChild[1], STDOUT_FILENO);
    close(ToChild[0]);
    close(ToChild[1]);
    close(FromChild[0]);
    close(FromChild[1]);
    execl("/bin/sh", "sh", "-c", Cmd.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  close(ToChild[0]);
  close(FromChild[1]);
  // A dead helper must surface as a write error, not kill llvm-exegesis.
  signal(SIGPIPE, SIG_IGN);
  std::unique_ptr<RemoteHelper> Helper(
      new RemoteHelper(Pid, ToChild[1], FromChild[0]));
  auto Hello = Helper->request("hello 1");
  if (!Hello)
    return Hello.takeError();
  if (!StringRef(*Hello).starts_with("1"))
    return make_error<Failure>("remote helper speaks protocol '" + *Hello +
                               "', expected 1");
  return std::move(Helper);
}

RemoteHelper::~RemoteHelper() {
  // Best effort: ask the helper to leave, then reap it.
  if (auto Bye = request("quit"); !Bye)
    consumeError(Bye.takeError());
  close(ToHelper);
  close(FromHelper);
  int Status;
  waitpid(Pid, &Status, 0);
}

Expected<std::string> RemoteHelper::readLine() {
  while (true) {
    size_t NewLine = Pending.find('\n');
    if (NewLine != std::string::npos) {
      std::string Line = Pending.substr(0, NewLine);
      Pending.erase(0, NewLine + 1);
      return Line;
    }
    char Buffer[4096];
    ssize_t N = read(FromHelper, Buffer, sizeof(Buffer));
    if (N < 0 && errno == EINTR)
      continue;
    if (N <= 0)
      return make_error<Failure>("the remote helper closed its output");
    Pending.append(Buffer, N);
  }
}

Expected<std::string> RemoteHelper::request(StringRef Line) {
  std::string Out = (Line + "\n").str();
  for (size_t Done = 0; Done < Out.size();) {
    ssize_t N = write(ToHelper, Out.data() + Done, Out.size() - Done);
    if (N < 0 && errno == EINTR)
      continue;
    if (N <= 0)
      return make_error<Failure>("cannot write to the remote helper");
    Done += N;
  }
  auto Reply = readLine();
  if (!Reply)
    return Reply.takeError();
  StringRef R = StringRef(*Reply).rtrim("\r");
  if (R == "ok")
    return std::string();
  if (R.consume_front("ok "))
    return R.str();
  R.consume_front("err ");
  return make_error<Failure>("remote helper: '" + Line.split(' ').first +
                             "' failed: " + R);
}
#else
Expected<std::unique_ptr<RemoteHelper>> RemoteHelper::launch(StringRef) {
  return make_error<Failure>(
      "the remote execution mode is only supported on Unix hosts");
}
RemoteHelper::~RemoteHelper() = default;
Expected<std::string> RemoteHelper::readLine() {
  llvm_unreachable("no helper on this host");
}
Expected<std::string> RemoteHelper::request(StringRef) {
  llvm_unreachable("no helper on this host");
}
#endif // LLVM_ON_UNIX

static std::string hex(uint64_t V) { return "0x" + utohexstr(V, true); }

Error RemoteHelper::writeMemory(uint64_t Address, ArrayRef<uint8_t> Data) {
  auto R = request("write " + hex(Address) + " " + toHex(Data, true));
  return R ? Error::success() : R.takeError();
}

Expected<std::vector<uint8_t>> RemoteHelper::readMemory(uint64_t Address,
                                                        uint64_t Size) {
  auto R = request("read " + hex(Address) + " " + hex(Size));
  if (!R)
    return R.takeError();
  std::string Bytes;
  if (R->size() != 2 * Size || !tryGetFromHex(*R, Bytes))
    return make_error<Failure>("remote helper: bad read reply '" + *R + "'");
  return std::vector<uint8_t>(Bytes.begin(), Bytes.end());
}

Expected<uint64_t>
RemoteHelper::run(uint64_t PC, uint64_t Stop, unsigned TimeoutMs,
                  ArrayRef<std::pair<std::string, uint64_t>> Registers) {
  std::string Line =
      "run " + hex(PC) + " " + hex(Stop) + " " + std::to_string(TimeoutMs);
  for (const auto &[Name, Value] : Registers)
    Line += " " + Name + "=" + hex(Value);
  auto R = request(Line);
  if (!R) {
    // A snippet that never returns (e.g. a branch to itself) is a property
    // of the snippet, not a broken session.
    std::string Msg = toString(R.takeError());
    if (StringRef(Msg).ends_with("failed: timeout"))
      return make_error<SnippetRemoteFailure>(
          "the snippet did not stop within " + Twine(TimeoutMs) + " ms");
    return make_error<Failure>(Msg);
  }
  StringRef Reply = *R;
  uint64_t Halted;
  if (!Reply.consume_front("halted ") || Reply.getAsInteger(0, Halted))
    return make_error<Failure>("remote helper: bad run reply '" + *R + "'");
  return Halted;
}

Expected<std::string> RemoteHelper::command(StringRef Text) {
  auto R = request("cmd " + Text.str());
  if (!R)
    return R.takeError();
  std::string Output;
  if (!tryGetFromHex(*R, Output))
    return make_error<Failure>("remote helper: bad cmd reply '" + *R + "'");
  return Output;
}

//===----------------------------------------------------------------------===//
// Session
//===----------------------------------------------------------------------===//

Expected<std::unique_ptr<RemoteSession>>
RemoteSession::create(StringRef HelperCommand, StringRef DescriptionPath,
                      const ExegesisTarget &ET) {
  if (!ET.getRemoteABI())
    return make_error<Failure>(
        "the remote execution mode is not supported for this target");
  auto Desc = RemoteTargetDescription::readFile(DescriptionPath);
  if (!Desc)
    return Desc.takeError();
  auto Helper = RemoteHelper::launch(HelperCommand);
  if (!Helper)
    return Helper.takeError();
  std::unique_ptr<RemoteSession> Session(
      new RemoteSession(std::move(*Desc), std::move(*Helper), ET));
  if (Error E = Session->initialize())
    return std::move(E);
  return std::move(Session);
}

Expected<std::unique_ptr<RemoteSession>>
RemoteSession::createOffline(StringRef DescriptionPath,
                             const ExegesisTarget &ET) {
  if (!ET.getRemoteABI())
    return make_error<Failure>(
        "the remote execution mode is not supported for this target");
  auto Desc = RemoteTargetDescription::readFile(DescriptionPath);
  if (!Desc)
    return Desc.takeError();
  return std::unique_ptr<RemoteSession>(
      new RemoteSession(std::move(*Desc), nullptr, ET));
}

Error RemoteSession::runSteps(
    ArrayRef<RemoteTargetDescription::InitStep> Steps) const {
  for (const RemoteTargetDescription::InitStep &Step : Steps) {
    if (Step.Monitor) {
      auto Out = Helper->command(*Step.Monitor);
      if (!Out)
        return Out.takeError();
      continue;
    }
    uint8_t Value[4];
    support::endian::write32le(Value, Step.Write32[1]);
    if (Error E = Helper->writeMemory(Step.Write32[0], Value))
      return E;
  }
  return Error::success();
}

Error RemoteSession::initialize() {
  if (Error E = runSteps(Desc.Init))
    return E;
  if (Error E = Helper->writeMemory(Desc.Scratch.Base,
                                    std::vector<uint8_t>(Desc.Scratch.Size, 0)))
    return E;
  return Helper->writeMemory(Desc.returnPadAddress(),
                             ET.getRemoteABI()->ReturnPad);
}

Expected<int64_t> RemoteSession::runOnce(const object::ObjectFile &Obj) const {
  if (!Helper)
    return make_error<Failure>("no remote helper in this session");
  // The code runs at an address chosen here, so it must not need relocating.
  // Relocations of other sections (e.g. .eh_frame) do not matter: only the
  // code is loaded.
  for (const object::SectionRef &Section : Obj.sections()) {
    if (Section.relocations().empty())
      continue;
    Expected<object::section_iterator> Relocated =
        Section.getRelocatedSection();
    if (!Relocated)
      return Relocated.takeError();
    if (*Relocated != Obj.section_end() && (*Relocated)->isText())
      return make_error<Failure>(
          "the benchmark function has relocations, which the remote "
          "execution mode does not support");
  }
  std::vector<uint8_t> Bytes;
  if (Error E = getBenchmarkFunctionBytes(Obj.getData(), Bytes))
    return std::move(E);
  if (Bytes.size() > Desc.Code.Size - RemoteTargetDescription::kFunctionOffset)
    return make_error<Failure>("the benchmark function (" +
                               Twine(Bytes.size()) +
                               " bytes) does not fit in the code region");

  const RemoteTargetDescription::Region &S = Desc.Scratch;
  static const std::vector<uint8_t> Zeros(RemoteTargetDescription::kResultsSize,
                                          0);
  if (Error E = Helper->writeMemory(Desc.functionAddress(), Bytes))
    return std::move(E);
  if (Error E = Helper->writeMemory(Desc.Results, Zeros))
    return std::move(E);
  // Unlike the in-process mode, scratch memory is cleared only once, in
  // initialize(): over a debug probe, clearing it before each run would cost
  // more than the run itself.

  const ExegesisTarget::RemoteABI ABI = *ET.getRemoteABI();
  std::vector<std::pair<std::string, uint64_t>> Registers = {
      {ABI.ArgumentRegister, S.Base},
      {ABI.StackPointer, Desc.StackTop},
      {ABI.LinkRegister, Desc.returnPadAddress() | ABI.LinkRegisterTag}};
  for (const auto &[Name, Value] : ABI.ExtraRegisters)
    Registers.emplace_back(Name, Value);
  auto Halted = Helper->run(Desc.functionAddress(), Desc.returnPadAddress(),
                            Desc.TimeoutMs, Registers);
  Error Failed = [&]() -> Error {
    if (!Halted)
      return Halted.takeError();
    if (*Halted != Desc.returnPadAddress())
      return make_error<SnippetRemoteFailure>(
          "the snippet stopped at " + hex(*Halted) +
          " instead of returning to the return pad (" +
          hex(Desc.returnPadAddress()) + "): it probably faulted");
    return Error::success();
  }();
  if (Failed) {
    // Bring the core back to a known state for the next run.
    if (Failed.isA<SnippetRemoteFailure>())
      if (Error E = runSteps(Desc.Recover))
        return joinErrors(std::move(Failed), std::move(E));
    return std::move(Failed);
  }

  auto Results =
      Helper->readMemory(Desc.Results, RemoteTargetDescription::kResultsSize);
  if (!Results)
    return Results.takeError();
  const uint64_t Start = support::endian::read64le(Results->data());
  const uint64_t End = support::endian::read64le(Results->data() + 8);
  const unsigned Width =
      Desc.Counter ? Desc.Counter->Width : ABI.CycleCounterWidth;
  const uint64_t Mask = Width >= 64 ? ~uint64_t(0) : (uint64_t(1) << Width) - 1;
  if (!Desc.Counter)
    return static_cast<int64_t>((End - Start) & Mask);
  const uint64_t Elapsed =
      (Desc.Counter->CountsDown ? Start - End : End - Start) & Mask;
  return static_cast<int64_t>(Elapsed * Desc.Counter->Scale);
}

} // namespace exegesis
} // namespace llvm
