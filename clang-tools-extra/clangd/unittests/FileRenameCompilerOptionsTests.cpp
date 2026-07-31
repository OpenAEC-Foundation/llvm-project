//===-- FileRenameCompilerOptionsTests.cpp -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CompilerInvocation.h"
#include "FileRename.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <string>
#include <vector>

namespace clang {
namespace clangd {
namespace {

using testing::HasSubstr;

tooling::CompileCommand command(llvm::ArrayRef<std::string> Args) {
  tooling::CompileCommand Command;
  Command.Directory = "/workspace";
  Command.Filename = "/workspace/main.cpp";
  Command.CommandLine.assign(Args.begin(), Args.end());
  return Command;
}

llvm::Error validate(llvm::ArrayRef<std::string> Args,
                     llvm::ArrayRef<std::pair<Path, Path>> Renames =
                         std::pair<Path, Path>{"/workspace/config",
                                               "/workspace/moved"},
                     llvm::ArrayRef<std::string> DerivedCC1 = {}) {
  tooling::CompileCommand Command;
  Command.Directory = "/workspace";
  Command.Filename = "/workspace/main.cpp";
  Command.CommandLine.assign(Args.begin(), Args.end());
  if (!llvm::is_contained(Command.CommandLine, "main.cpp"))
    Command.CommandLine.push_back("main.cpp");
  return validateCompileCommandForRenames(Command, Renames, {}, nullptr,
                                          DerivedCC1);
}

TEST(FileRenameCompilerOptions, RejectsDriverGeneratedSearchRoots) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{"clang", "-cxx-isystem",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-stdlib++-isystem",
                                    "/workspace/config"},
           std::vector<std::string>{"clang",
                                    "--gcc-install-dir=/workspace/config"},
           std::vector<std::string>{"clang", "--cuda-path=/workspace/config"},
           std::vector<std::string>{"clang", "--hip-path=/workspace/config"},
           std::vector<std::string>{"clang", "--rocm-path=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "--hipstdpar-path=/workspace/config"},
           std::vector<std::string>{
               "clang", "--hipstdpar-thrust-path=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "--hipstdpar-prim-path=/workspace/config"},
           std::vector<std::string>{
               "clang", "--libomptarget-amdgpu-bc-path=/workspace/config"},
           std::vector<std::string>{
               "clang", "--libomptarget-nvptx-bc-path=/workspace/config"},
           std::vector<std::string>{
               "clang", "--libomptarget-spirv-bc-path=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "--rocm-device-lib-path=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "--hip-device-lib=/workspace/config"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args),
                      llvm::FailedWithMessage(HasSubstr("compiler")));
  }
}

TEST(FileRenameCompilerOptions, RejectsSemanticDriverFiles) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{
               "clang", "-mzos-sys-include=/other:/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fbuild-session-file=/workspace/config"},
           std::vector<std::string>{"clang", "-Xclang",
                                    "-fmodules-embed-file=/workspace/config"},
           std::vector<std::string>{
               "clang", "--warning-suppression-mappings=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "--multi-lib-config=/workspace/config"},
           std::vector<std::string>{"clang", "-fplugin=/workspace/config"},
           std::vector<std::string>{"clang", "-fpass-plugin=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "--hipspv-pass-plugin=/workspace/config"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args),
                      llvm::FailedWithMessage(HasSubstr("compiler")));
  }
}

TEST(FileRenameCompilerOptions, ParsesClangCLMode) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{"clang-cl", "/I/workspace/config"},
           std::vector<std::string>{"clang-cl", "/external:I/workspace/config"},
           std::vector<std::string>{"clang-cl", "/imsvc/workspace/config"},
           std::vector<std::string>{"clang-cl", "/FI/workspace/config"},
           std::vector<std::string>{"clang-cl", "/winsysroot/workspace/config"},
           std::vector<std::string>{"clang-cl", "/vctoolsdir/workspace/config"},
           std::vector<std::string>{"clang-cl", "/winsdkdir/workspace/config"},
           std::vector<std::string>{"clang-cl", "/diasdkdir/workspace/config"},
           std::vector<std::string>{"clang-cl", "/Fp/workspace/config"},
           std::vector<std::string>{"clang-cl", "/Ycconfig"},
           std::vector<std::string>{"clang-cl", "/Yuconfig"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args),
                      llvm::FailedWithMessage(HasSubstr("compiler")));
  }
}

TEST(FileRenameCompilerOptions, ParsesForwardedAndDirectCC1Options) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{"clang", "-Xpreprocessor",
                                    "-I/workspace/config"},
           std::vector<std::string>{"clang", "-Wp,-I,/workspace/config"},
           std::vector<std::string>{"clang", "-cc1", "-ast-merge",
                                    "/workspace/config/state.ast"},
           std::vector<std::string>{"clang", "-cc1", "-remap-file",
                                    "/workspace/config/from;/workspace/other"},
           std::vector<std::string>{"clang", "-cc1", "-remap-file",
                                    "/workspace/other;/workspace/config/to"},
           std::vector<std::string>{
               "clang", "-cc1",
               "-foverride-record-layout=/workspace/config/layout.txt"},
           std::vector<std::string>{"clang", "-cc1", "-load",
                                    "/workspace/config/plugin.so"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args),
                      llvm::FailedWithMessage(HasSubstr("compiler path")));
  }
  EXPECT_THAT_ERROR(
      validate({"clang-cl", "/clang:-I/workspace/config"}),
      llvm::FailedWithMessage(HasSubstr("unsupported compiler invocation")));
}

TEST(FileRenameCompilerOptions, RejectsUnrepresentedTargetJobs) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{"clang", "-Xarch_host",
                                    "-I/workspace/config"},
           std::vector<std::string>{"clang", "-Xarch_device",
                                    "-I/workspace/config"},
           std::vector<std::string>{"clang", "-Xopenmp-target",
                                    "-I/workspace/config"},
           std::vector<std::string>{"clang", "-Xoffload-compiler",
                                    "-I/workspace/config"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(
        validate(Args, {{"/workspace/config", "/workspace/moved"}},
                 {"-cc1", "-I", "/workspace/other"}),
        llvm::FailedWithMessage(HasSubstr("target-selected compiler jobs")));
  }
}

TEST(FileRenameCompilerOptions, ChecksDriverDerivedCommand) {
  EXPECT_THAT_ERROR(
      validate({"clang", "main.cpp"},
               {{"/workspace/config", "/workspace/moved"}},
               {"-cc1", "-internal-isystem", "/workspace/config"}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
}

TEST(FileRenameCompilerOptions, ResolvesPrefixesAndSysrootsInOrder) {
  EXPECT_THAT_ERROR(
      validate(
          {"clang", "-iprefix", "/workspace/prefix/", "-iwithprefix", "config"},
          {{"/workspace/prefix/config", "/workspace/moved"}}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
  EXPECT_THAT_ERROR(
      validate({"clang", "-iprefix", "/workspace/prefix/", "-iprefix",
                "/workspace/other/", "-iwithprefix", "config"},
               {{"/workspace/prefix/config", "/workspace/moved"}}),
      llvm::Succeeded());
  EXPECT_THAT_ERROR(
      validate({"clang", "--sysroot=/workspace/root", "-I=config"},
               {{"/workspace/root/config", "/workspace/moved"}}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
  EXPECT_THAT_ERROR(
      validate({"clang", "--sysroot=/workspace/root", "-B", "=config"},
               {{"/workspace/root/config/tool", "/workspace/moved"}}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
}

TEST(FileRenameCompilerOptions, RejectsUnexpandedConfigurationInputs) {
  EXPECT_THAT_ERROR(validate({"clang", "@flags.rsp"}),
                    llvm::FailedWithMessage(HasSubstr("response file")));
  EXPECT_THAT_ERROR(validate({"clang", "--config=/workspace/config"}),
                    llvm::FailedWithMessage(HasSubstr("configuration file")));
  EXPECT_THAT_ERROR(validate({"/workspace/config/clang", "main.cpp"}),
                    llvm::FailedWithMessage(HasSubstr("compiler path")));
}

TEST(FileRenameCompilerOptions, RecognizesOnlySupportedDriverNamesAndModes) {
  for (llvm::StringRef Name :
       {"clang", "clang-cpp", "clang++-20", "gcc", "g++", "c++", "clang-cl"}) {
    SCOPED_TRACE(Name);
    auto Result = normalizeCompilerCommand(command({Name.str(), "main.cpp"}));
    EXPECT_THAT_EXPECTED(Result, llvm::Succeeded());
  }
  std::string TargetDriver = llvm::sys::getDefaultTargetTriple();
  TargetDriver += "-clang++";
  EXPECT_THAT_EXPECTED(
      normalizeCompilerCommand(command({TargetDriver, "main.cpp"})),
      llvm::Succeeded());
  for (llvm::StringRef Name :
       {"unknowncc", "wrapper-clang", "clang-dxc", "flang"}) {
    SCOPED_TRACE(Name);
    EXPECT_THAT_EXPECTED(
        normalizeCompilerCommand(command({Name.str(), "main.cpp"})),
        llvm::Failed());
  }
  EXPECT_THAT_EXPECTED(normalizeCompilerCommand(
                           command({"clang", "--driver-mode=dxc", "main.cpp"})),
                       llvm::Failed());
}

TEST(FileRenameCompilerOptions, LocatesOnlyParsedSourceOperands) {
  auto AfterDash = normalizeCompilerCommand(
      command({"clang", "-Dmain.cpp=1", "--", "main.cpp"}));
  ASSERT_THAT_EXPECTED(AfterDash, llvm::Succeeded());
  ASSERT_EQ(AfterDash->Inputs.size(), 1u);
  EXPECT_EQ(AfterDash->Inputs.front().ArgumentIndex, 3u);
  EXPECT_EQ(AfterDash->Inputs.front().ValueOffset, 0u);

  auto Joined = normalizeCompilerCommand(
      command({"clang-cl", "/Tcmain.cpp", "/link", "other.cpp"}));
  ASSERT_THAT_EXPECTED(Joined, llvm::Succeeded());
  EXPECT_EQ(Joined->Mode, CompilerInvocationMode::ClangCL);
  EXPECT_EQ(Joined->Inputs.front().ArgumentIndex, 1u);
  EXPECT_EQ(Joined->Inputs.front().ValueOffset, 3u);

  auto Separate = normalizeCompilerCommand(
      command({"clang-cl", "/Tp", "main.cpp", "/link", "other.cpp"}));
  ASSERT_THAT_EXPECTED(Separate, llvm::Succeeded());
  EXPECT_EQ(Separate->Inputs.front().ArgumentIndex, 2u);
  EXPECT_EQ(Separate->Inputs.front().ValueOffset, 0u);

  EXPECT_THAT_EXPECTED(
      normalizeCompilerCommand(command({"clang", "a.cpp", "b.cpp"})),
      llvm::FailedWithMessage(HasSubstr("multiple source inputs")));

  auto DirectCC1 = normalizeCompilerCommand(
      command({"clang", "-cc1", "-x", "c++", "main.cpp"}));
  ASSERT_THAT_EXPECTED(DirectCC1, llvm::Succeeded());
  EXPECT_EQ(DirectCC1->Mode, CompilerInvocationMode::DirectCC1);
  EXPECT_EQ(DirectCC1->Inputs.front().ArgumentIndex, 4u);

  auto ExplicitLanguage =
      normalizeCompilerCommand(command({"clang", "-x", "c++", "main.cpp"}));
  ASSERT_THAT_EXPECTED(ExplicitLanguage, llvm::Succeeded());
  EXPECT_EQ(ExplicitLanguage->Inputs.front().ArgumentIndex, 3u);

  auto Proxy = command({"clang", "main.cpp"});
  Proxy.Filename = "/workspace/proxy.h";
  auto ProxyResult = normalizeCompilerCommand(Proxy);
  ASSERT_THAT_EXPECTED(ProxyResult, llvm::Succeeded());
  ASSERT_EQ(ProxyResult->Inputs.size(), 1u);
  EXPECT_EQ(ProxyResult->Inputs.front().AbsolutePath, "/workspace/main.cpp");

  EXPECT_THAT_EXPECTED(normalizeCompilerCommand(command({"clang", "-"})),
                       llvm::FailedWithMessage(HasSubstr("stdin")));
}

TEST(FileRenameCompilerOptions, RecordsLastEffectiveWorkingDirectory) {
  auto Result = normalizeCompilerCommand(
      command({"clang", "-working-directory", "/workspace/first",
               "-working-directory=/workspace/last", "../main.cpp"}));
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  EXPECT_EQ(Result->EffectiveDirectory, "/workspace/last");
  ASSERT_EQ(Result->WorkingDirectories.size(), 2u);
  EXPECT_EQ(Result->WorkingDirectories.back().ArgumentIndex, 3u);
  EXPECT_EQ(Result->WorkingDirectories.back().ValueOffset,
            llvm::StringRef("-working-directory=").size());
}

TEST(FileRenameCompilerOptions, CompileCommandEqualityIncludesProvenance) {
  auto Left = command({"clang", "main.cpp"});
  auto Right = Left;
  EXPECT_EQ(Left, Right);
  Right.HadConfigFile = true;
  EXPECT_NE(Left, Right);
  Right = Left;
  Right.HadResponseFile = true;
  EXPECT_NE(Left, Right);
}

} // namespace
} // namespace clangd
} // namespace clang
