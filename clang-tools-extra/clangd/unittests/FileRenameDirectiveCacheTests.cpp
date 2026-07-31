//===-- FileRenameDirectiveCacheTests.cpp -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FileRenameInternal.h"
#include "TestFS.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Testing/Support/Error.h"
#include "gtest/gtest.h"
#include <memory>
#include <mutex>
#include <utility>

namespace clang {
namespace clangd {
namespace {

class CountingFileSystem : public MockFS {
  class View : public llvm::vfs::ProxyFileSystem {
  public:
    View(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> Base,
         const CountingFileSystem &Owner)
        : ProxyFileSystem(std::move(Base)), Owner(Owner) {}

    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
    openFileForRead(const llvm::Twine &Path) override {
      {
        std::lock_guard<std::mutex> Lock(Owner.Mu);
        ++Owner.Reads[Path.str()];
      }
      return ProxyFileSystem::openFileForRead(Path);
    }

  private:
    const CountingFileSystem &Owner;
  };

public:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> viewImpl() const override {
    return new View(MockFS::viewImpl(), *this);
  }

  unsigned reads(PathRef File) const {
    std::lock_guard<std::mutex> Lock(Mu);
    return Reads.lookup(File);
  }

private:
  mutable std::mutex Mu;
  mutable llvm::StringMap<unsigned> Reads;
};

TEST(FileRenameDirectiveCache, ReusesCanonicalScanAcrossContextsAndAliases) {
  CountingFileSystem TFS;
  const Path File = testPath("include/shared.h");
  const Path Alias = testPath("include/sub/../shared.h");
  TFS.Files[File] = "#if ENABLED\n#include \"target.h\"\n#endif\n";
  auto FS = TFS.view(std::nullopt);
  FileDigest Expected = digest(TFS.Files.lookup(File));
  FileRenameDirectiveCache Cache(*FS);

  for (unsigned Context = 0; Context != 100; ++Context)
    ASSERT_THAT_EXPECTED(Cache.scan(File, Expected), llvm::Succeeded());
  ASSERT_THAT_EXPECTED(Cache.scan(Alias, Expected), llvm::Succeeded());
  EXPECT_EQ(TFS.reads(File), 1U);
  EXPECT_EQ(TFS.reads(Alias), 0U);

  EXPECT_THAT_EXPECTED(Cache.scan(Alias, digest("different")),
                       llvm::FailedWithMessage(testing::HasSubstr(
                           "aliases have different contents")));
  EXPECT_EQ(TFS.reads(File), 1U);
  EXPECT_EQ(TFS.reads(Alias), 0U);
}

TEST(FileRenameDirectiveCache, FailedFrozenDigestCheckDoesNotCommit) {
  CountingFileSystem TFS;
  const Path File = testPath("shared.h");
  TFS.Files[File] = "#include \"target.h\"\n";
  auto FS = TFS.view(std::nullopt);
  FileRenameDirectiveCache Cache(*FS);

  EXPECT_THAT_EXPECTED(
      Cache.scan(File, digest("stale")),
      llvm::FailedWithMessage(testing::HasSubstr("frozen digest")));
  EXPECT_EQ(TFS.reads(File), 1U);
  ASSERT_THAT_EXPECTED(Cache.scan(File, digest(TFS.Files.lookup(File))),
                       llvm::Succeeded());
  EXPECT_EQ(TFS.reads(File), 2U);
}

} // namespace
} // namespace clangd
} // namespace clang
