//===- ExpandResponseFilesCompilationDatabaseTest.cpp --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Tooling/CompilationDatabase.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "gtest/gtest.h"
#include <memory>
#include <utility>
#include <vector>

namespace clang {
namespace tooling {
namespace {

class SingleCommandDatabase final : public CompilationDatabase {
public:
  explicit SingleCommandDatabase(CompileCommand Command)
      : Command(std::move(Command)) {}

  std::vector<CompileCommand>
  getCompileCommands(llvm::StringRef File) const override {
    return File == Command.Filename ? std::vector<CompileCommand>{Command}
                                    : std::vector<CompileCommand>{};
  }

private:
  CompileCommand Command;
};

CompileCommand expand(CompileCommand Command,
                      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS) {
  const std::string File = Command.Filename;
  auto Results = expandResponseFiles(std::make_unique<SingleCommandDatabase>(
                                         std::move(Command)),
                                     std::move(FS))
                     ->getCompileCommands(File);
  EXPECT_EQ(Results.size(), 1u);
  return Results.empty() ? CompileCommand() : std::move(Results.front());
}

CompileCommand command(std::vector<std::string> Arguments) {
  return CompileCommand("/", "/main.cpp", std::move(Arguments), "");
}

TEST(ExpandResponseFilesProvenance, RecordsAllObservedIndirection) {
  auto FS = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
  ASSERT_TRUE(
      FS->addFile("/args.rsp", 0, llvm::MemoryBuffer::getMemBuffer("-DVALUE")));

  auto Expanded = expand(command({"clang", "@/args.rsp", "/main.cpp"}), FS);
  EXPECT_TRUE(Expanded.HadResponseFile);
  EXPECT_EQ(Expanded.CommandLine,
            (std::vector<std::string>{"clang", "-DVALUE", "/main.cpp"}));

  EXPECT_FALSE(expand(command({"clang", "/main.cpp"}), FS).HadResponseFile);
  EXPECT_TRUE(expand(command({"clang", "@/missing.rsp", "/main.cpp"}), FS)
                  .HadResponseFile);

  auto Preserved = command({"clang", "/main.cpp"});
  Preserved.HadResponseFile = true;
  EXPECT_TRUE(expand(std::move(Preserved), FS).HadResponseFile);
}

} // namespace
} // namespace tooling
} // namespace clang
