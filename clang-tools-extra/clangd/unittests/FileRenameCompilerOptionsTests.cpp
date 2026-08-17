//===-- FileRenameCompilerOptionsTests.cpp -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CompilerInvocation.h"
#include "FileRename.h"
#include "TestFS.h"
#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <cstdlib>
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
  if (Command.CommandLine.size() > 1 && Command.CommandLine[1] == "-cc1")
    Command.CommandLine.insert(Command.CommandLine.begin() + 2,
                               "-fsyntax-only");
  else if (!Command.CommandLine.empty() &&
           llvm::StringRef(Command.CommandLine.front()).contains("clang-cl"))
    Command.CommandLine.insert(Command.CommandLine.begin() + 1, "/c");
  else if (!Command.CommandLine.empty())
    Command.CommandLine.insert(Command.CommandLine.begin() + 1, "-c");
  if (!llvm::is_contained(Command.CommandLine, "main.cpp"))
    Command.CommandLine.push_back("main.cpp");
  return validateCompileCommandForRenames(Command, Renames, {}, nullptr,
                                          DerivedCC1);
}

TEST(FileRenameCompilerOptions, RejectsNonClangAndLinkCommands) {
  EXPECT_THAT_ERROR(
      validate({"gcc", "main.cpp"}),
      llvm::FailedWithMessage(HasSubstr("requires a Clang compiler")));

  auto Link = command({"clang", "main.cpp"});
  EXPECT_THAT_ERROR(
      validateCompileCommandForRenames(
          Link, {{"/workspace/config", "/workspace/moved"}}, {}, nullptr, {}),
      llvm::FailedWithMessage(HasSubstr("compile-only")));
}

TEST(FileRenameCompilerOptions, RejectsCommandsWithMultipleFrontendJobs) {
  auto Command = command({"clang", "--target=x86_64-apple-darwin", "-arch",
                          "x86_64", "-arch", "arm64", "-c", "main.cpp"});
  EXPECT_THAT_ERROR(
      validateCompileCommandForRenames(Command, {}, {}, nullptr, {}),
      llvm::FailedWithMessage(HasSubstr("compiler jobs")));
}

TEST(FileRenameCompilerOptions, RejectsDependencyContainersAndFileQueries) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{"clang", "-ivfsoverlay",
                                    "/workspace/overlay.yaml"},
           std::vector<std::string>{"clang",
                                    "-fmodule-map-file=/workspace/map.txt"},
           std::vector<std::string>{"clang", "-DHAS=__has_include(\"old.h\")"},
           std::vector<std::string>{"clang", "-fuse-ld=custom-ld"},
           std::vector<std::string>{"clang", "--ld-path=/workspace/ld"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args), llvm::Failed());
  }
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
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args),
                      llvm::FailedWithMessage(HasSubstr("compiler")));
  }
}

TEST(FileRenameCompilerOptions, RejectsUnprovenPlugins) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{"clang", "-fplugin=/workspace/plugin.so"},
           std::vector<std::string>{"clang",
                                    "-fpass-plugin=/workspace/plugin.so"},
           std::vector<std::string>{
               "clang", "--hipspv-pass-plugin=/workspace/plugin.so"},
           std::vector<std::string>{"clang", "-Xclang", "-load", "-Xclang",
                                    "/workspace/plugin.so"},
           std::vector<std::string>{"clang", "-Xclang", "-add-plugin",
                                    "-Xclang", "custom"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args), llvm::FailedWithMessage(
                                          HasSubstr("compiler plugin option")));
  }
}

TEST(FileRenameCompilerOptions, RejectsPrecompiledInputs) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{"clang", "-include-pch",
                                    "/workspace/prefix.pch"},
           std::vector<std::string>{"clang",
                                    "-fmodule-file=Foo=/workspace/foo.pcm"},
           std::vector<std::string>{
               "clang", "-fprebuilt-module-path=/workspace/modules"},
           std::vector<std::string>{"clang", "-Xclang", "-ast-merge", "-Xclang",
                                    "/workspace/merge.ast"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args), llvm::FailedWithMessage(HasSubstr(
                                          "precompiled compiler input")));
  }
}

TEST(FileRenameCompilerOptions, RejectsOpaqueDownstreamOptions) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{"clang", "-mllvm", "-config=old.cfg"},
           std::vector<std::string>{"clang", "-Xassembler", "old.cfg"},
           std::vector<std::string>{"clang", "-Wa,@old.rsp"},
           std::vector<std::string>{"clang", "-Xlinker", "old.ld"},
           std::vector<std::string>{"clang", "-Wl,-T,/workspace/old.ld"},
           std::vector<std::string>{"clang", "-Xcuda-ptxas", "old.cfg"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args), llvm::FailedWithMessage(
                                          HasSubstr("opaque compiler option")));
  }
}

TEST(FileRenameCompilerOptions, RejectsAllExplicitSemanticInputFiles) {
  for (const std::vector<std::string> &Args : {
           std::vector<std::string>{"clang",
                                    "-fembed-offload-object=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fprofile-sample-use=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fprofile-instr-use=/workspace/config"},
           std::vector<std::string>{
               "clang", "-fprofile-remapping-file=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fprofile-list=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fcodegen-data-use=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fmemory-profile-use=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fsanitize-ignorelist=/workspace/config"},
           std::vector<std::string>{
               "clang", "-fsanitize-system-ignorelist=/workspace/config"},
           std::vector<std::string>{
               "clang", "-fsanitize-coverage-allowlist=/workspace/config"},
           std::vector<std::string>{
               "clang", "-fsanitize-coverage-ignorelist=/workspace/config"},
           std::vector<std::string>{
               "clang",
               "-fexperimental-sanitize-metadata-ignorelist=/workspace/config"},
           std::vector<std::string>{
               "clang", "-frandomize-layout-seed-file=/workspace/config"},
           std::vector<std::string>{
               "clang", "-fxray-always-instrument=/workspace/config"},
           std::vector<std::string>{
               "clang", "-fxray-never-instrument=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fxray-attr-list=/workspace/config"},
           std::vector<std::string>{
               "clang",
               "-fms-secure-hotpatch-functions-file=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fthinlto-index=/workspace/config"},
           std::vector<std::string>{
               "clang", "--extract-api-ignores=/other,/workspace/config"},
       }) {
    SCOPED_TRACE(llvm::join(Args, " "));
    EXPECT_THAT_ERROR(validate(Args),
                      llvm::FailedWithMessage(HasSubstr("compiler path")));
  }

  for (const std::vector<std::string> &CC1Args : {
           std::vector<std::string>{"-cc1", "-mlink-builtin-bitcode",
                                    "/workspace/config"},
           std::vector<std::string>{"-cc1", "-mlink-bitcode-file",
                                    "/workspace/config"},
           std::vector<std::string>{
               "-cc1", "-fprofile-instrument-use-path=/workspace/config"},
           std::vector<std::string>{"-cc1", "-fcuda-include-gpubinary",
                                    "/workspace/config"},
           std::vector<std::string>{"-cc1", "-fopenmp-host-ir-file-path",
                                    "/workspace/config"},
       }) {
    SCOPED_TRACE(llvm::join(CC1Args, " "));
    EXPECT_THAT_ERROR(validate({"clang", "main.cpp"},
                               {{"/workspace/config", "/workspace/moved"}},
                               CC1Args),
                      llvm::FailedWithMessage(HasSubstr("compiler path")));
  }
}

TEST(FileRenameCompilerOptions, RejectsImplicitAndDirectoryProfileInputs) {
  EXPECT_THAT_ERROR(
      validate({"clang", "-fprofile-instr-use"},
               {{"/workspace/default.profdata", "/workspace/moved"}}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
  EXPECT_THAT_ERROR(
      validate({"clang", "-fprofile-use=/workspace/profiles"},
               {{"/workspace/profiles/default.profdata", "/workspace/moved"}}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
}

TEST(FileRenameCompilerOptions, ResolvesBareCompilerExecutable) {
  auto Compiler = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(Compiler) << Compiler.getError().message();
  EXPECT_THAT_ERROR(validate({"clang", "main.cpp"},
                             {{*Compiler, "/workspace/replacement-clang"}}),
                    llvm::FailedWithMessage(HasSubstr("compiler executable")));
  EXPECT_THAT_ERROR(
      validate({"aarch64-unknown-linux-gnu-clang", "main.cpp"}),
      llvm::FailedWithMessage(HasSubstr("cannot resolve compiler executable")));
  EXPECT_THAT_ERROR(
      validate({"clang", "main.cpp"}, {{"/workspace/old", "/workspace/clang"}}),
      llvm::FailedWithMessage(HasSubstr("executable resolution")));

  MockFS TFS;
  TFS.Files["/workspace/stage/clang"] = "compiler";
  auto FS = TFS.view(std::nullopt);
  auto Status = FS->status("/workspace/stage/clang");
  ASSERT_TRUE(Status);
  auto Command = command({"clang", "-c", "main.cpp"});
  FileRenameMapping Expanded{"/workspace/stage/clang", "/workspace/bin/clang",
                             Status->getUniqueID()};
  EXPECT_THAT_ERROR(
      validateCompileCommandForRenames(Command,
                                       {{"/workspace/stage", "/workspace/bin"}},
                                       {Expanded}, FS.get(), {}),
      llvm::FailedWithMessage(HasSubstr("executable resolution")));
}

#if LLVM_ON_UNIX
TEST(FileRenameCompilerOptions, RejectsClangSymlinkToGCCOnPath) {
  auto GCC = llvm::sys::findProgramByName("gcc");
  ASSERT_TRUE(GCC) << GCC.getError().message();
  llvm::SmallString<256> Root;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory(
      "clangd-rename-compiler-identity", Root));
  llvm::scope_exit Cleanup([&] { llvm::sys::fs::remove_directories(Root); });
  llvm::SmallString<256> FakeClang(Root);
  llvm::sys::path::append(FakeClang, "clang");
  ASSERT_FALSE(llvm::sys::fs::create_link(*GCC, FakeClang));

  ASSERT_TRUE(::getenv("PATH"));
  llvm::scope_exit RestorePath([Old = std::string(::getenv("PATH"))] {
    ::setenv("PATH", Old.c_str(), /*overwrite=*/1);
  });
  ::setenv("PATH", Root.str().str().c_str(), /*overwrite=*/1);
  EXPECT_THAT_ERROR(
      validate({"clang", "main.cpp"}),
      llvm::FailedWithMessage(HasSubstr("requires a Clang compiler")));
}
#endif

TEST(FileRenameCompilerOptions, CanonicalizesBothSidesOfTreeRoots) {
  llvm::SmallString<256> Workspace;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("clangd-rename-tree-root",
                                                    Workspace));
  llvm::scope_exit Cleanup(
      [&] { llvm::sys::fs::remove_directories(Workspace); });
  auto Path = [&](llvm::StringRef Name) {
    llvm::SmallString<256> Result(Workspace);
    llvm::sys::path::append(Result, Name);
    return Result.str().str();
  };
  const std::string Root = Path("toolchain");
  const std::string Alias = Path("toolchain-alias");
  ASSERT_FALSE(llvm::sys::fs::create_directory(Root));
  ASSERT_FALSE(llvm::sys::fs::create_link(Root, Alias));

  const std::string OldInside = Path("toolchain/old.cfg");
  const std::string Outside = Path("outside.cfg");
  for (PathRef File : {PathRef(OldInside), PathRef(Outside)}) {
    int FD;
    ASSERT_FALSE(llvm::sys::fs::openFileForWrite(File, FD));
    llvm::raw_fd_ostream Stream(FD, /*shouldClose=*/true);
  }

  tooling::CompileCommand Command;
  Command.Directory = Workspace.str().str();
  Command.Filename = Path("main.cpp");
  Command.CommandLine = {"clang", "-c", "-B", Alias, Command.Filename};
  auto FS = llvm::vfs::getRealFileSystem();
  EXPECT_THAT_ERROR(
      validateCompileCommandForRenames(
          Command, {{OldInside, Path("moved.cfg")}}, {}, FS.get(), {}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
  EXPECT_THAT_ERROR(
      validateCompileCommandForRenames(
          Command, {{Outside, Path("toolchain/new.cfg")}}, {}, FS.get(), {}),
      llvm::FailedWithMessage(HasSubstr("compiler path namespace")));
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
           std::vector<std::string>{"clang", "-cc1", "-remap-file",
                                    "/workspace/config/from;/workspace/other"},
           std::vector<std::string>{"clang", "-cc1", "-remap-file",
                                    "/workspace/other;/workspace/config/to"},
           std::vector<std::string>{
               "clang", "-cc1",
               "-foverride-record-layout=/workspace/config/layout.txt"},
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

TEST(FileRenameCompilerOptions, RejectsOpaqueAnalyzerAndSSAFInputs) {
  for (llvm::StringRef Setting :
       {"Config", "ctu-dir", "ctu-index-name", "model-path"}) {
    SCOPED_TRACE(Setting);
    std::string Value = (Setting + "=/workspace/config").str();
    EXPECT_THAT_ERROR(
        validate({"clang", "-Xclang", "-analyzer-config", "-Xclang", Value}),
        llvm::FailedWithMessage(HasSubstr("analyzer configuration")));
    EXPECT_THAT_ERROR(
        validate({"clang", "-cc1", "-analyzer-config", Value}),
        llvm::FailedWithMessage(HasSubstr("analyzer configuration")));
  }
  EXPECT_THAT_ERROR(
      validate(
          {"clang", "--ssaf-global-scope-analysis-result=/workspace/config"}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
  EXPECT_THAT_ERROR(
      validate({"clang", "-cc1",
                "--ssaf-global-scope-analysis-result=/workspace/config"}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
  EXPECT_THAT_ERROR(
      validate({"clang", "-Xanalyzer", "-analyzer-config"}),
      llvm::FailedWithMessage(HasSubstr("opaque compiler option")));
}

TEST(FileRenameCompilerOptions, RejectsCommandMacroDependencyState) {
  for (llvm::StringRef Macro : {"-DALIAS=_Pragma(\"include_alias(old,new)\")",
                                "-DALIAS=__pragma(include_alias(old,new))"}) {
    SCOPED_TRACE(Macro);
    EXPECT_THAT_ERROR(
        validate({"clang", Macro.str()}),
        llvm::FailedWithMessage(HasSubstr("preprocessor dependency state")));
  }
  EXPECT_THAT_ERROR(
      validate({"clang", "-DQUERY=CAT(__has_,include)"}),
      llvm::FailedWithMessage(HasSubstr("preprocessor dependency state")));
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
  ASSERT_TRUE(Result->WorkingDirectory);
  EXPECT_EQ(Result->WorkingDirectory->ArgumentIndex, 3u);
  EXPECT_EQ(Result->WorkingDirectory->ValueOffset,
            llvm::StringRef("-working-directory=").size());
}

TEST(FileRenameCompilerOptions, ProjectsCommandThroughDirectoryRename) {
  auto Command = command({"clang", "-working-directory", "/workspace/old/build",
                          "-c", "../main.cpp"});
  Command.Directory = "/workspace/old";
  Command.Filename = "/workspace/old/main.cpp";
  auto Projected = projectCompileCommandAfterRenames(
      Command, {{"/workspace/old", "/workspace/new"}});
  ASSERT_THAT_EXPECTED(Projected, llvm::Succeeded());
  EXPECT_EQ(Projected->Directory, "/workspace/new");
  EXPECT_EQ(Projected->Filename, "/workspace/new/main.cpp");
  EXPECT_THAT(Projected->CommandLine,
              testing::ElementsAre("clang", "-working-directory",
                                   "/workspace/new/build", "-c",
                                   "../main.cpp"));
}

TEST(FileRenameCompilerOptions, ResolvesPathsFromLastWorkingDirectory) {
  auto Command = command({"clang", "-working-directory", "/workspace/first",
                          "-working-directory=/workspace/project", "-Iinclude",
                          "-c", "main.cpp"});
  Command.Directory = "/workspace/build";
  EXPECT_THAT_ERROR(
      validateCompileCommandForRenames(
          Command, {{"/workspace/project/include", "/workspace/project/moved"}},
          {}, nullptr, {}),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
  EXPECT_THAT_ERROR(
      validateCompileCommandForRenames(
          Command, {{"/workspace/build/include", "/workspace/build/moved"}}, {},
          nullptr, {}),
      llvm::Succeeded());
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
