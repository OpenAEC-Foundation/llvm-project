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
  std::vector<CompilerInputArgument> WorkingDirectories;
};

llvm::Expected<NormalizedCompilerCommand>
normalizeCompilerCommand(const tooling::CompileCommand &Command);

CompilerInvocationMode
compilerInvocationMode(llvm::ArrayRef<std::string> CommandLine);

/// Whether building this driver command now loads a configuration file.
bool compilerLoadsConfigFile(const tooling::CompileCommand &Command);

} // namespace clangd
} // namespace clang

#endif
