//===--- CompilerInvocation.cpp - Classify Clang command lines --*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CompilerInvocation.h"
#include "SourceCode.h"
#include "support/Logger.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Options/Options.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Path.h"
#include "llvm/TargetParser/Triple.h"
#include <cstdint>

namespace clang {
namespace clangd {
namespace {

bool supportedDriverName(llvm::StringRef Executable) {
  driver::ParsedClangName Parsed =
      driver::ToolChain::getTargetAndModeFromProgramName(Executable);
  if (Parsed.isEmpty() || (!Parsed.TargetPrefix.empty() &&
                           llvm::Triple(Parsed.TargetPrefix).getArch() ==
                               llvm::Triple::UnknownArch))
    return false;
  // getTargetAndModeFromProgramName deliberately recognizes any executable
  // ending in e.g. "cc". Limit this to the actual driver names we support.
  return llvm::is_contained(
      llvm::ArrayRef<llvm::StringLiteral>{
          "clang", "clang++", "clang-c++", "clang-cc", "clang-cpp", "clang-g++",
          "clang-gcc", "clang-cl", "gcc", "g++", "cc", "c++", "cpp", "cl"},
      Parsed.ModeSuffix);
}

llvm::Expected<CompilerInvocationMode>
invocationMode(llvm::ArrayRef<std::string> CommandLine) {
  if (CommandLine.empty())
    return error("compiler command line is empty");
  if (CommandLine.size() > 1 && CommandLine[1] == "-cc1")
    return CompilerInvocationMode::DirectCC1;
  llvm::SmallVector<const char *> Args;
  for (const std::string &Arg : CommandLine)
    Args.push_back(Arg.c_str());
  llvm::StringRef Mode =
      driver::getDriverMode(Args.front(), llvm::ArrayRef(Args).drop_front());
  if (Mode == "cl")
    return CompilerInvocationMode::ClangCL;
  if (Mode.empty() || Mode == "gcc" || Mode == "g++" || Mode == "cpp")
    return CompilerInvocationMode::GCCDriver;
  return error("unsupported compiler driver mode {0}", Mode);
}

Path absolutePath(llvm::StringRef Value, PathRef Directory) {
  llvm::SmallString<256> Result(Value);
  if (!llvm::sys::path::is_absolute(Result)) {
    Result = Directory;
    llvm::sys::path::append(Result, Value);
  }
  llvm::sys::path::remove_dots(Result, /*remove_dot_dot=*/true);
  return Result.str().str();
}

llvm::Expected<CompilerInputArgument>
locateValueSpan(llvm::StringRef Value, Path AbsolutePath, unsigned FirstArg,
                unsigned End, llvm::ArrayRef<const char *> Raw,
                const tooling::CompileCommand &Command) {
  uintptr_t Address = reinterpret_cast<uintptr_t>(Value.data());
  for (unsigned I = FirstArg; I != End; ++I) {
    uintptr_t Begin = reinterpret_cast<uintptr_t>(Raw[I]);
    if (Address < Begin)
      continue;
    uintptr_t RawOffset = Address - Begin;
    if (RawOffset > Command.CommandLine[I].size())
      continue;
    size_t Offset = static_cast<size_t>(RawOffset);
    if (Value.size() > Command.CommandLine[I].size() - Offset)
      continue;
    return CompilerInputArgument{I, static_cast<unsigned>(Offset),
                                 static_cast<unsigned>(Value.size()),
                                 std::move(AbsolutePath)};
  }
  return error("cannot locate compiler operand spelling {0}", Value);
}

} // namespace

CompilerInvocationMode
compilerInvocationMode(llvm::ArrayRef<std::string> CommandLine) {
  auto Mode = invocationMode(CommandLine);
  return Mode ? *Mode : CompilerInvocationMode::GCCDriver;
}

bool compilerLoadsConfigFile(const tooling::CompileCommand &Command) {
  if (Command.CommandLine.empty() ||
      compilerInvocationMode(Command.CommandLine) ==
          CompilerInvocationMode::DirectCC1)
    return false;
  std::vector<const char *> Args;
  for (const std::string &Arg : Command.CommandLine)
    Args.push_back(Arg.c_str());
  bool HadConfigFile = false;
  CreateInvocationOptions Options;
  Options.RecoverOnError = true;
  Options.HadConfigFile = &HadConfigFile;
  (void)createInvocation(Args, std::move(Options));
  return HadConfigFile;
}

llvm::Expected<unsigned>
compilerDriverJobCount(const tooling::CompileCommand &Command) {
  if (Command.CommandLine.empty())
    return error("compiler command line is empty");
  if (compilerInvocationMode(Command.CommandLine) ==
      CompilerInvocationMode::DirectCC1)
    return 1u;
  std::vector<const char *> Args;
  for (const std::string &Arg : Command.CommandLine)
    Args.push_back(Arg.c_str());
  unsigned JobCount = 0;
  CreateInvocationOptions Options;
  Options.RecoverOnError = true;
  Options.DriverJobCount = &JobCount;
  if (!createInvocation(Args, std::move(Options)))
    return error("cannot derive compiler jobs from command");
  return JobCount;
}

llvm::Expected<NormalizedCompilerCommand>
normalizeCompilerCommand(const tooling::CompileCommand &Command) {
  if (Command.CommandLine.empty())
    return error("compiler command line is empty");
  if (Command.HadResponseFile)
    return error("compiler command used response-file indirection");
  if (Command.HadConfigFile)
    return error("compiler command used configuration-file indirection");
  if (!supportedDriverName(Command.CommandLine.front()))
    return error("unsupported compiler launcher {0}",
                 Command.CommandLine.front());
  for (llvm::StringRef Arg : Command.CommandLine) {
    if (Arg == "-cc1as" || Arg == "-fc1" || Arg == "--driver-mode=dxc" ||
        Arg == "--driver-mode=flang" || Arg.starts_with("/clang:"))
      return error("unsupported compiler invocation mode {0}", Arg);
    if (Arg.starts_with("@"))
      return error("compiler command has unexpanded response file {0}", Arg);
  }

  NormalizedCompilerCommand Result;
  auto Mode = invocationMode(Command.CommandLine);
  if (!Mode)
    return Mode.takeError();
  Result.Mode = *Mode;
  unsigned FirstArg = Result.Mode == CompilerInvocationMode::DirectCC1 ? 2 : 1;
  if (Command.CommandLine.size() < FirstArg)
    return error("malformed direct cc1 command");
  llvm::SmallVector<const char *> Raw;
  for (const std::string &Arg : Command.CommandLine)
    Raw.push_back(Arg.c_str());
  unsigned End = Command.CommandLine.size();
  if (Result.Mode == CompilerInvocationMode::ClangCL)
    for (unsigned I = FirstArg; I != End; ++I)
      if (llvm::StringRef(Command.CommandLine[I]).equals_insensitive("/link")) {
        End = I;
        break;
      }
  unsigned MissingIndex = 0, MissingCount = 0;
  auto Parsed = getDriverOptTable().ParseArgs(
      llvm::ArrayRef(Raw).slice(FirstArg, End - FirstArg), MissingIndex,
      MissingCount,
      llvm::opt::Visibility(Result.Mode == CompilerInvocationMode::ClangCL
                                ? options::CLOption
                            : Result.Mode == CompilerInvocationMode::DirectCC1
                                ? options::CC1Option
                                : options::ClangOption));
  if (MissingCount)
    return error("compiler option {0} has no argument",
                 Command.CommandLine[FirstArg + MissingIndex]);
  if (Parsed.hasArg(options::OPT_config))
    return error("compiler command has explicit configuration file");

  Result.EffectiveDirectory = Command.Directory;
  if (const llvm::opt::Arg *Arg =
          Parsed.getLastArg(options::OPT_working_directory)) {
    Result.EffectiveDirectory =
        absolutePath(Arg->getValue(), Command.Directory);
    auto Span = locateValueSpan(Arg->getValue(), Result.EffectiveDirectory,
                                FirstArg, End, Raw, Command);
    if (!Span)
      return Span.takeError();
    Result.WorkingDirectory = std::move(*Span);
  }

  auto RecordInput =
      [&](llvm::StringRef Value) -> llvm::Expected<CompilerInputArgument> {
    if (Value == "-")
      return error("compiler input from stdin cannot be renamed");
    return locateValueSpan(Value,
                           absolutePath(Value, Result.EffectiveDirectory),
                           FirstArg, End, Raw, Command);
  };
  for (const llvm::opt::Arg *Arg : Parsed) {
    if (Arg->getOption().matches(options::OPT__DASH_DASH)) {
      for (llvm::StringRef Value : Arg->getValues()) {
        auto Input = RecordInput(Value);
        if (!Input)
          return Input.takeError();
        Result.Inputs.push_back(std::move(*Input));
      }
      continue;
    }
    if (!Arg->getOption().matches(options::OPT_INPUT) &&
        !Arg->getOption().matches(options::OPT__SLASH_Tc) &&
        !Arg->getOption().matches(options::OPT__SLASH_Tp))
      continue;
    if (Arg->getNumValues() != 1)
      return error("compiler input has ambiguous path count");
    auto Input = RecordInput(Arg->getValue());
    if (!Input)
      return Input.takeError();
    Result.Inputs.push_back(std::move(*Input));
  }
  if (Result.Inputs.empty())
    return error("compiler command has no source input");
  if (Result.Inputs.size() != 1)
    return error("compiler command has multiple source inputs");
  return Result;
}

} // namespace clangd
} // namespace clang
