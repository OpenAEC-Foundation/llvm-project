//===--- CompilerInvocation.h - Classify Clang command lines ----*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_COMPILERINVOCATION_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_COMPILERINVOCATION_H

#include "support/Path.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/Support/Error.h"
#include <optional>
#include <utility>
#include <vector>

namespace clang {
namespace clangd {

enum class CompilerInvocationMode { GCCDriver, ClangCL, DirectCC1 };

struct CompilerInputArgument {
  unsigned ArgumentIndex = 0;
  unsigned ValueOffset = 0;
  unsigned ValueLength = 0;
  Path AbsolutePath;
};

struct NormalizedCompilerCommand {
  CompilerInvocationMode Mode = CompilerInvocationMode::GCCDriver;
  Path EffectiveDirectory;
  std::vector<CompilerInputArgument> Inputs;
  std::optional<CompilerInputArgument> WorkingDirectory;
};

llvm::Expected<NormalizedCompilerCommand>
normalizeCompilerCommand(const tooling::CompileCommand &Command);

/// Rewrites the paths that identify a compile command and its sole source
/// input as they will appear after \p Renames.
///
/// This preserves relative source and working-directory spellings whenever
/// they still resolve to the mapped paths. The command must be accepted by
/// normalizeCompilerCommand().
llvm::Expected<tooling::CompileCommand> projectCompileCommandAfterRenames(
    const tooling::CompileCommand &Command,
    llvm::ArrayRef<std::pair<Path, Path>> Renames);

CompilerInvocationMode
compilerInvocationMode(llvm::ArrayRef<std::string> CommandLine);

/// Whether building this driver command now loads a configuration file.
bool compilerLoadsConfigFile(const tooling::CompileCommand &Command);

/// Number of compiler jobs selected by the driver for this command.
llvm::Expected<unsigned>
compilerDriverJobCount(const tooling::CompileCommand &Command);

} // namespace clangd
} // namespace clang

#endif
