//===-- FileRenameTests.cpp -----------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Compiler.h"
#include "Diagnostics.h"
#include "FileRename.h"
#include "ParsedAST.h"
#include "SourceCode.h"
#include "TestFS.h"
#include "TestTU.h"
#include "support/Logger.h"
#include "clang/Format/Format.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <utility>

namespace clang {
namespace clangd {
namespace {
using testing::HasSubstr;

llvm::Expected<std::vector<TextEdit>>
editsFor(TestTU &TU, llvm::ArrayRef<std::pair<Path, Path>> Renames) {
  MockFS FS;
  auto Inputs = TU.inputs(FS);
  StoreDiags Diags;
  auto CI = buildCompilerInvocation(Inputs, Diags);
  if (!CI)
    return error("failed to build test compiler invocation");
  auto AST = ParsedAST::build(testPath(TU.Filename), Inputs, std::move(CI),
                              Diags.take(), /*Preamble=*/nullptr);
  if (!AST)
    return error("failed to build test AST");
  auto VFS = FS.view(std::nullopt);
  std::vector<FileRenameMapping> Mappings;
  for (const auto &[Old, New] : Renames) {
    auto OldStatus = VFS->status(Old);
    if (!OldStatus)
      return error("missing test input {0}", Old);
    Mappings.push_back({Old, New, OldStatus->getUniqueID()});
  }
  return renameIncludeDirectives(
      testPath(TU.Filename), TU.Code, AST->getIncludeStructure(),
      AST->getPreprocessor().getHeaderSearchInfo(),
      Inputs.CompileCommand.Directory, Mappings, format::getLLVMStyle(), *VFS);
}

llvm::Expected<std::vector<TextEdit>> editsFor(TestTU &TU, PathRef Old,
                                               PathRef New) {
  std::pair<Path, Path> Rename{Old.str(), New.str()};
  return editsFor(TU, {Rename});
}

TEST(FileRename, RenamesQuotedIncludeByResolvedIdentity) {
  TestTU TU;
  TU.Code = "#include \"old.h\"\n";
  TU.AdditionalFiles["old.h"] = "";
  auto Result = editsFor(TU, testPath("old.h"), testPath("new.h"));
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  ASSERT_EQ(Result->size(), 1u);
  EXPECT_EQ(Result->front().newText, "\"new.h\"");
  EXPECT_EQ(Result->front().range, (Range{{0, 9}, {0, 16}}));
}

TEST(FileRename, DoesNotRenameAHeaderWithOnlyTheSameSpelling) {
  TestTU TU;
  TU.Filename = "source/main.cpp";
  TU.Code = "#include \"same.h\"\n";
  TU.AdditionalFiles["source/same.h"] = "";
  TU.AdditionalFiles["other/same.h"] = "";
  auto Result =
      editsFor(TU, testPath("other/same.h"), testPath("other/renamed.h"));
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  EXPECT_THAT(*Result, testing::IsEmpty());
}

TEST(FileRename, RejectsMacroGeneratedIncludeOperand) {
  TestTU TU;
  TU.Code = "#define HEADER \"old.h\"\n#include HEADER\n";
  TU.AdditionalFiles["old.h"] = "";
  auto Result = editsFor(TU, testPath("old.h"), testPath("new.h"));
  ASSERT_THAT_EXPECTED(Result,
                       llvm::FailedWithMessage(HasSubstr("macro-generated")));
}

TEST(FileRename, RenamesAngledIncludeUsingHeaderSearch) {
  TestTU TU;
  TU.Code = "#include <old.h>\n";
  TU.AdditionalFiles["include/old.h"] = "";
  TU.ExtraArgs = {"-I", testPath("include")};
  auto Result =
      editsFor(TU, testPath("include/old.h"), testPath("include/new.h"));
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  ASSERT_EQ(Result->size(), 1u);
  EXPECT_EQ(Result->front().newText, "<new.h>");
}

TEST(FileRename, RejectsAnIncludeThatWouldResolveToAnotherFile) {
  TestTU TU;
  TU.Code = "#include <old.h>\n";
  TU.AdditionalFiles["first/new.h"] = "";
  TU.AdditionalFiles["second/old.h"] = "";
  TU.ExtraArgs = {"-I", testPath("first"), "-I", testPath("second")};
  auto Result =
      editsFor(TU, testPath("second/old.h"), testPath("second/new.h"));
  EXPECT_THAT_EXPECTED(
      Result, llvm::FailedWithMessage(HasSubstr("would resolve to file")));
}

TEST(FileRename, RejectsDestinationThatShadowsUnchangedInclude) {
  TestTU TU;
  TU.Code = "#include <current.h>\n";
  TU.AdditionalFiles["first/unrelated.h"] = "";
  TU.AdditionalFiles["second/current.h"] = "";
  TU.ExtraArgs = {"-I", testPath("first"), "-I", testPath("second")};
  auto Result = editsFor(
      TU, testPath("first/unrelated.h"), testPath("first/current.h"));
  EXPECT_THAT_EXPECTED(
      Result,
      llvm::FailedWithMessage(HasSubstr("instead of")));
}

TEST(FileRename, AllowsShadowThatMovesAwayInSameTransaction) {
  TestTU TU;
  TU.Code = "#include <old.h>\n";
  TU.AdditionalFiles["first/new.h"] = "";
  TU.AdditionalFiles["second/old.h"] = "";
  TU.ExtraArgs = {"-I", testPath("first"), "-I", testPath("second")};
  auto Result = editsFor(
      TU, {{testPath("second/old.h"), testPath("second/new.h")},
           {testPath("first/new.h"), testPath("first/moved-away.h")}});
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  ASSERT_THAT(*Result, testing::SizeIs(1));
  EXPECT_EQ(Result->front().newText, "<new.h>");
}

TEST(FileRename, RejectsInvalidGeneratedHeaderName) {
  TestTU TU;
  TU.Code = "#include \"old.h\"\n";
  TU.AdditionalFiles["old.h"] = "";
  auto Result = editsFor(TU, testPath("old.h"), testPath("new\"name.h"));
  EXPECT_THAT_EXPECTED(
      Result, llvm::FailedWithMessage(HasSubstr("header-name token")));
}

TEST(FileRename, RenamesObjCImport) {
  TestTU TU;
  TU.Filename = "main.m";
  TU.Code = "#import \"old.h\"\n";
  TU.AdditionalFiles["old.h"] = "";
  auto Result = editsFor(TU, testPath("old.h"), testPath("new.h"));
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  ASSERT_EQ(Result->size(), 1u);
  EXPECT_EQ(Result->front().newText, "\"new.h\"");
}

TEST(FileRename, RewritesIncludesRelativeToAMovedIncluder) {
  TestTU TU;
  TU.Filename = "old/main.cpp";
  TU.Code = "#include \"header.h\"\n";
  TU.AdditionalFiles["old/header.h"] = "";
  std::pair<Path, Path> Rename{testPath("old/main.cpp"),
                               testPath("new/main.cpp")};
  auto Result = editsFor(TU, {Rename});
  ASSERT_THAT_EXPECTED(Result, llvm::Succeeded());
  ASSERT_EQ(Result->size(), 1u);
  EXPECT_EQ(Result->front().newText, "\"../old/header.h\"");
}

TEST(FileRename, RejectsUnresolvedIncludesAtomically) {
  TestTU TU;
  TU.Code = "#include \"missing.h\" // error-ok\n";
  TU.AdditionalFiles["old.h"] = "";
  auto Result = editsFor(TU, testPath("old.h"), testPath("new.h"));
  EXPECT_THAT_EXPECTED(
      Result, llvm::FailedWithMessage(HasSubstr("unresolved include")));
}

TEST(FileRename, ExpandsDirectoriesAndRejectsConflictingMappings) {
  MockFS FS;
  FS.Files[testPath("old/a.h")] = "";
  FS.Files[testPath("old/nested/b.h")] = "";
  FS.Files[testPath("old/.git/ignored.h")] = "";
  FS.Files[testPath("old/.hg/ignored.h")] = "";
  FS.Files[testPath("old/.svn/ignored.h")] = "";
  auto VFS = FS.view(std::nullopt);
  auto Expanded =
      expandFileRenames({{testPath("old"), testPath("new")}}, testRoot(), *VFS);
  ASSERT_THAT_EXPECTED(Expanded, llvm::Succeeded());
  EXPECT_EQ(Expanded->size(), 2u);
  EXPECT_THAT(*Expanded, testing::UnorderedElementsAre(
                             testing::Field(&FileRenameMapping::NewPath,
                                            testPath("new/a.h")),
                             testing::Field(&FileRenameMapping::NewPath,
                                            testPath("new/nested/b.h"))));

  auto Metadata = expandFileRenames(
      {{testPath("old/.git"), testPath("metadata")}}, testRoot(), *VFS);
  ASSERT_THAT_EXPECTED(Metadata, llvm::Succeeded());
  EXPECT_TRUE(Metadata->empty());

  auto Conflict =
      expandFileRenames({{testPath("old/a.h"), testPath("new/a.h")},
                         {testPath("old/a.h"), testPath("other/a.h")}},
                        testRoot(), *VFS);
  EXPECT_THAT_EXPECTED(
      Conflict, llvm::FailedWithMessage(HasSubstr("same file is renamed")));
}

TEST(FileRename, NormalizesMultipleAndOverlappingMappings) {
  MockFS FS;
  FS.Files[testPath("old/a.h")] = "";
  FS.Files[testPath("second.h")] = "";
  auto VFS = FS.view(std::nullopt);
  auto Expanded =
      expandFileRenames({{testPath("old/./"), testPath("new/../new")},
                         {testPath("old/a.h"), testPath("new/a.h")},
                         {testPath("second.h"), testPath("renamed-second.h")}},
                        testRoot(), *VFS);
  ASSERT_THAT_EXPECTED(Expanded, llvm::Succeeded());
  EXPECT_THAT(*Expanded, testing::UnorderedElementsAre(
                             testing::Field(&FileRenameMapping::NewPath,
                                            testPath("new/a.h")),
                             testing::Field(&FileRenameMapping::NewPath,
                                            testPath("renamed-second.h"))));
}

TEST(FileRename, RejectsExistingDestinationAndOutsideWorkspace) {
  MockFS FS;
  FS.Files[testPath("old.h")] = "";
  FS.Files[testPath("existing.h")] = "";
  auto VFS = FS.view(std::nullopt);
  EXPECT_THAT_EXPECTED(
      expandFileRenames({{testPath("old.h"), testPath("existing.h")}},
                        testRoot(), *VFS),
      llvm::FailedWithMessage(HasSubstr("destination already exists")));

  llvm::SmallString<128> Outside(testRoot());
  llvm::sys::path::remove_filename(Outside);
  llvm::sys::path::append(Outside, "outside.h");
  EXPECT_THAT_EXPECTED(
      expandFileRenames({{testPath("old.h"), Outside.str().str()}}, testRoot(),
                        *VFS),
      llvm::FailedWithMessage(HasSubstr("outside the workspace")));
}

TEST(FileRename, RejectsInvalidDirectoryDestinations) {
  MockFS FS;
  FS.Files[testPath("old/file.h")] = "";
  FS.Files[testPath("existing/other.h")] = "";
  auto VFS = FS.view(std::nullopt);
  EXPECT_THAT_EXPECTED(
      expandFileRenames({{testPath("old"), testPath("existing")}}, testRoot(),
                        *VFS),
      llvm::FailedWithMessage(HasSubstr("destination already exists")));
  EXPECT_THAT_EXPECTED(
      expandFileRenames({{testPath("old"), testPath("old/nested")}}, testRoot(),
                        *VFS),
      llvm::FailedWithMessage(HasSubstr("inside its source directory")));
}

TEST(FileRename, RejectsSymlinkEscapesFromWorkspace) {
  llvm::SmallString<256> Workspace;
  llvm::SmallString<256> Outside;
  ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("clangd-rename-workspace",
                                                    Workspace));
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("clangd-rename-outside", Outside));
  llvm::scope_exit Cleanup([&] {
    llvm::sys::fs::remove_directories(Workspace);
    llvm::sys::fs::remove_directories(Outside);
  });
  llvm::SmallString<256> InsideFile(Workspace);
  llvm::sys::path::append(InsideFile, "inside.h");
  llvm::SmallString<256> OutsideFile(Outside);
  llvm::sys::path::append(OutsideFile, "out.h");
  for (PathRef File : {InsideFile.str(), OutsideFile.str()}) {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(File, EC);
    ASSERT_FALSE(EC);
  }
  llvm::SmallString<256> Escape(Workspace);
  llvm::sys::path::append(Escape, "escape");
  ASSERT_FALSE(llvm::sys::fs::create_symlink(Outside, Escape));
  auto FS = llvm::vfs::getRealFileSystem();

  llvm::SmallString<256> EscapedSource(Escape);
  llvm::sys::path::append(EscapedSource, "out.h");
  llvm::SmallString<256> Renamed(Workspace);
  llvm::sys::path::append(Renamed, "renamed.h");

  EXPECT_THAT_EXPECTED(
      expandFileRenames({{{EscapedSource.str().str()}, {Renamed.str().str()}}},
                        Workspace, *FS),
      llvm::FailedWithMessage(HasSubstr("outside the workspace")));
  llvm::SmallString<256> EscapedDestination(Escape);
  llvm::sys::path::append(EscapedDestination, "renamed.h");
  EXPECT_THAT_EXPECTED(
      expandFileRenames(
          {{{InsideFile.str().str()}, {EscapedDestination.str().str()}}},
          Workspace, *FS),
      llvm::FailedWithMessage(HasSubstr("outside the workspace")));
}

TEST(FileRename, UsesFilesystemCaseSensitivityForDestinationCollisions) {
  llvm::SmallString<256> Workspace;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("clangd-rename-case", Workspace));
  llvm::scope_exit Cleanup(
      [&] { llvm::sys::fs::remove_directories(Workspace); });
  auto Path = [&](llvm::StringRef Name) {
    llvm::SmallString<256> Result(Workspace);
    llvm::sys::path::append(Result, Name);
    return Result.str().str();
  };
  for (PathRef File : {Path("A.h"), Path("a.h")}) {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(File, EC);
    ASSERT_FALSE(EC);
  }
  auto FS = llvm::vfs::getRealFileSystem();

  auto Expanded = expandFileRenames(
      {{{Path("A.h")}, {Path("New.h")}}, {{Path("a.h")}, {Path("new.h")}}},
      Workspace, *FS);
  ASSERT_THAT_EXPECTED(Expanded, llvm::Succeeded());
  EXPECT_EQ(Expanded->size(), 2u);
}

TEST(FileRename, RejectsMovedCompilerConfigurationPaths) {
  tooling::CompileCommand Command;
  Command.Directory = "/workspace";
  Command.Filename = "/workspace/main.cpp";
  const std::pair<Path, Path> Rename{"/workspace/config", "/workspace/moved"};
  for (std::vector<std::string> Args : {
           std::vector<std::string>{"clang", "-I", "/workspace/config"},
           std::vector<std::string>{"clang", "-iquote/workspace/config"},
           std::vector<std::string>{"clang", "--sysroot=/workspace/config"},
           std::vector<std::string>{"clang", "-resource-dir=/workspace/config"},
           std::vector<std::string>{"clang", "-include",
                                    "/workspace/config/prefix.h"},
           std::vector<std::string>{"clang", "-Xclang", "-ivfsoverlay",
                                    "-Xclang", "/workspace/config/vfs.yaml"},
           std::vector<std::string>{"clang",
                                    "-fmodule-map-file=/workspace/config"},
  }) {
    Args.push_back("-c");
    Args.push_back(Command.Filename);
    Command.CommandLine = std::move(Args);
    EXPECT_THAT_ERROR(
        validateCompileCommandForRenames(Command, {Rename}),
        llvm::FailedWithMessage(testing::AnyOf(
            HasSubstr("compiler path"),
            HasSubstr("precompiled compiler input"))));
  }
}

TEST(FileRename, RejectsAllIncludeModuleAndVFSCompilerPaths) {
  tooling::CompileCommand Command;
  Command.Directory = "/workspace";
  Command.Filename = "/workspace/main.cpp";
  const std::pair<Path, Path> Rename{"/workspace/config", "/workspace/moved"};
  for (std::vector<std::string> Args : {
           std::vector<std::string>{"clang", "-I", "/workspace/config"},
           std::vector<std::string>{"clang", "-F/workspace/config"},
           std::vector<std::string>{"clang", "--embed-dir=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "--gcc-toolchain=/workspace/config"},
           std::vector<std::string>{"clang", "--sysroot=/workspace/config"},
           std::vector<std::string>{"clang", "-resource-dir",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-resource-dir=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fmodule-map-file=/workspace/config/map"},
           std::vector<std::string>{
               "clang", "-fmodule-file=Named=/workspace/config/module.pcm"},
           std::vector<std::string>{"clang",
                                    "-fprebuilt-module-path=/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-fmodules-cache-path=/workspace/config"},
           std::vector<std::string>{"clang", "-fmodules-user-build-path",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-include",
                                    "/workspace/config/header.h"},
           std::vector<std::string>{"clang", "-include-pch",
                                    "/workspace/config/header.pch"},
           std::vector<std::string>{"clang", "-imacros",
                                    "/workspace/config/macros.h"},
           std::vector<std::string>{"clang", "-iprefix", "/workspace/config"},
           std::vector<std::string>{"clang", "-iwithprefix/workspace/config"},
           std::vector<std::string>{"clang", "-iwithprefixbefore",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-iwithsysroot/workspace/config"},
           std::vector<std::string>{"clang", "-iquote", "/workspace/config"},
           std::vector<std::string>{"clang", "-isysroot", "/workspace/config"},
           std::vector<std::string>{"clang", "-isystem", "/workspace/config"},
           std::vector<std::string>{"clang", "-isystem-after",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-idirafter", "/workspace/config"},
           std::vector<std::string>{"clang", "-iframework",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-iframeworkwithsysroot",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-iapinotes-modules",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-ivfsoverlay",
                                    "/workspace/config/vfs.yaml"},
           std::vector<std::string>{"clang", "-vfsoverlay",
                                    "/workspace/config/vfs.yaml"},
           std::vector<std::string>{"clang", "-working-directory",
                                    "/workspace/config"},
           std::vector<std::string>{"clang",
                                    "-working-directory=/workspace/config"},
           std::vector<std::string>{"clang", "-Xclang", "-c-isystem", "-Xclang",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-Xclang", "-objc-isystem",
                                    "-Xclang", "/workspace/config"},
           std::vector<std::string>{"clang", "-Xclang", "-objcxx-isystem",
                                    "-Xclang", "/workspace/config"},
           std::vector<std::string>{"clang", "-Xclang", "-internal-iframework",
                                    "-Xclang", "/workspace/config"},
           std::vector<std::string>{"clang", "-Xclang", "-internal-isystem",
                                    "-Xclang", "/workspace/config"},
           std::vector<std::string>{"clang", "-Xclang",
                                    "-internal-externc-isystem", "-Xclang",
                                    "/workspace/config"},
           std::vector<std::string>{"clang", "-Xclang", "-chain-include",
                                    "-Xclang", "/workspace/config/header.pch"},
  }) {
    Args.push_back("-c");
    Args.push_back(Command.Filename);
    Command.CommandLine = std::move(Args);
    EXPECT_THAT_ERROR(
        validateCompileCommandForRenames(Command, {Rename}),
        llvm::FailedWithMessage(testing::AnyOf(
            HasSubstr("compiler path"),
            HasSubstr("precompiled compiler input"))));
  }
}

TEST(FileRename, ResolvesIncludePrefixOptionsBeforeValidation) {
  tooling::CompileCommand Command;
  Command.Directory = "/workspace/build";
  Command.Filename = "/workspace/main.cpp";
  for (llvm::StringRef WithPrefix : {"-iwithprefix", "-iwithprefixbefore"}) {
    Command.CommandLine = {"clang",          "-c",       "-iprefix",
                           "/workspace/sdk/", WithPrefix.str(), "include",
                           Command.Filename};
    EXPECT_THAT_ERROR(
        validateCompileCommandForRenames(
            Command, {{"/workspace/sdk/include", "/workspace/sdk/renamed"}}),
        llvm::FailedWithMessage(HasSubstr("compiler path")));
  }
}

TEST(FileRename, ResolvesSysrootSearchOptionsBeforeValidation) {
  tooling::CompileCommand Command;
  Command.Directory = "/workspace/build";
  Command.Filename = "/workspace/main.cpp";
  for (std::vector<std::string> Args : {
           std::vector<std::string>{"clang", "-isysroot", "/workspace/sdk",
                                    "-iwithsysroot", "/include"},
           std::vector<std::string>{"clang", "--sysroot=/workspace/sdk",
                                    "-iframeworkwithsysroot", "/include"},
           std::vector<std::string>{"clang", "-isysroot", "/workspace/sdk",
                                    "-I=include"},
  }) {
    Args.push_back("-c");
    Args.push_back(Command.Filename);
    Command.CommandLine = std::move(Args);
    EXPECT_THAT_ERROR(
        validateCompileCommandForRenames(
            Command, {{"/workspace/sdk/include", "/workspace/sdk/renamed"}}),
        llvm::FailedWithMessage(HasSubstr("compiler path")));
  }
}

TEST(FileRename, RejectsMovedWorkingDirectoryAndResponseFiles) {
  tooling::CompileCommand Command;
  Command.Directory = "/workspace/config";
  Command.Filename = "/workspace/config/main.cpp";
  Command.CommandLine = {"clang", "-c", "main.cpp"};
  const std::pair<Path, Path> Rename{"/workspace/config", "/workspace/moved"};
  EXPECT_THAT_ERROR(validateCompileCommandForRenames(Command, {Rename}),
                    llvm::FailedWithMessage(HasSubstr("working directory")));

  Command.Directory = "/workspace";
  Command.CommandLine = {"clang", "-c", "@config/arguments.rsp"};
  EXPECT_THAT_ERROR(validateCompileCommandForRenames(Command, {Rename}),
                    llvm::FailedWithMessage(HasSubstr("response file")));
}

TEST(FileRename, RejectsCompilerPathAliasingRenamedFile) {
  llvm::SmallString<256> Workspace;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("clangd-rename-alias", Workspace));
  llvm::scope_exit Cleanup(
      [&] { llvm::sys::fs::remove_directories(Workspace); });
  auto FilePath = [&](llvm::StringRef Name) {
    llvm::SmallString<256> Result(Workspace);
    llvm::sys::path::append(Result, Name);
    return Result.str().str();
  };
  const std::string Config = FilePath("config.h");
  const std::string Alias = FilePath("alias.h");
  const std::string Renamed = FilePath("new.h");
  {
    std::error_code EC;
    llvm::raw_fd_ostream Stream(Config, EC);
    ASSERT_FALSE(EC);
  }
  ASSERT_FALSE(llvm::sys::fs::create_hard_link(Config, Alias));
  auto FS = llvm::vfs::getRealFileSystem();
  auto ConfigStatus = FS->status(Config);
  ASSERT_TRUE(ConfigStatus);
  FileRenameMapping Mapping{Config, Renamed, ConfigStatus->getUniqueID()};
  tooling::CompileCommand Command;
  Command.Directory = Workspace.str().str();
  Command.Filename = FilePath("main.cpp");
  Command.CommandLine = {"clang", "-c", "-include", Alias,
                         Command.Filename};
  const std::pair<Path, Path> Rename{Config, Renamed};

  EXPECT_THAT_ERROR(
      validateCompileCommandForRenames(Command, {Rename}, {Mapping}, FS.get()),
      llvm::FailedWithMessage(HasSubstr("compiler path")));
}

TEST(FileRename, DetectsConditionalIncludeDirectives) {
  MockFS FS;
  FS.Files[testPath("conditional.h")] = R"cpp(
#if ENABLED
#include "selected.h"
#endif
)cpp";
  FS.Files[testPath("unconditional.h")] = "#include \"always.h\"\n";
  FS.Files[testPath("guarded.h")] = R"cpp(
#ifndef GUARDED_H
#define GUARDED_H
#include "ordinary.h"
#endif
)cpp";
  FS.Files[testPath("nested-in-guard.h")] = R"cpp(
#ifndef NESTED_IN_GUARD_H
#define NESTED_IN_GUARD_H
#if ENABLED
#include "selected.h"
#endif
#endif
)cpp";
  auto VFS = FS.view(std::nullopt);

  auto Conditional =
      conditionalIncludeDirectives(testPath("conditional.h"), *VFS);
  ASSERT_THAT_EXPECTED(Conditional, llvm::Succeeded());
  EXPECT_THAT(*Conditional,
              testing::ElementsAre(testing::Field(
                  &ConditionalInclusion::Written, "\"selected.h\"")));
  auto Unconditional =
      conditionalIncludeDirectives(testPath("unconditional.h"), *VFS);
  ASSERT_THAT_EXPECTED(Unconditional, llvm::Succeeded());
  EXPECT_THAT(*Unconditional, testing::IsEmpty());
  auto Guarded = conditionalIncludeDirectives(testPath("guarded.h"), *VFS);
  ASSERT_THAT_EXPECTED(Guarded, llvm::Succeeded());
  EXPECT_THAT(*Guarded,
              testing::ElementsAre(testing::Field(
                  &ConditionalInclusion::Written, "\"ordinary.h\"")));
  auto Nested =
      conditionalIncludeDirectives(testPath("nested-in-guard.h"), *VFS);
  ASSERT_THAT_EXPECTED(Nested, llvm::Succeeded());
  EXPECT_THAT(*Nested, testing::ElementsAre(testing::Field(
                           &ConditionalInclusion::Written, "\"selected.h\"")));
}

TEST(FileRename, RejectsIncompatibleTranslationUnitEdits) {
  TextEdit First{Range{{0, 9}, {0, 16}}, "\"first.h\""};
  TextEdit Second{Range{{0, 9}, {0, 16}}, "\"second.h\""};
  EXPECT_THAT_ERROR(
      validateCompatibleFileRenameEdits("common.h", {First}, {First}),
      llvm::Succeeded());
  EXPECT_THAT_ERROR(
      validateCompatibleFileRenameEdits("common.h", {First}, {Second}),
      llvm::FailedWithMessage(HasSubstr("incompatible include edits")));
}

} // namespace
} // namespace clangd
} // namespace clang
