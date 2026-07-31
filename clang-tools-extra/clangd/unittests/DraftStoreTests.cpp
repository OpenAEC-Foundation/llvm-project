//===-- DraftStoreTests.cpp -------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DraftStore.h"
#include "llvm/ADT/STLExtras.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace clang {
namespace clangd {
namespace {

TEST(DraftStore, Versions) {
  DraftStore DS;
  Path File = "foo.cpp";

  EXPECT_EQ("25", DS.addDraft(File, "25", ""));
  EXPECT_EQ("25", DS.getDraft(File)->Version);
  EXPECT_EQ("", *DS.getDraft(File)->Contents);

  EXPECT_EQ("26", DS.addDraft(File, "", "x"));
  EXPECT_EQ("26", DS.getDraft(File)->Version);
  EXPECT_EQ("x", *DS.getDraft(File)->Contents);

  EXPECT_EQ("27", DS.addDraft(File, "", "x")) << "no-op change";
  EXPECT_EQ("27", DS.getDraft(File)->Version);
  EXPECT_EQ("x", *DS.getDraft(File)->Contents);

  // We allow versions to go backwards.
  EXPECT_EQ("7", DS.addDraft(File, "7", "y"));
  EXPECT_EQ("7", DS.getDraft(File)->Version);
  EXPECT_EQ("y", *DS.getDraft(File)->Contents);
}

TEST(DraftStore, AtomicSnapshotContainsContentsAndVersions) {
  DraftStore DS;
  DS.addDraft("a.cpp", "3", "old a");
  DS.addDraft("b.cpp", "7", "old b");

  auto Snapshot = DS.getDrafts();
  DS.addDraft("a.cpp", "4", "new a");
  DS.removeDraft("b.cpp");

  ASSERT_EQ(Snapshot.size(), 2u);
  auto Find = [&](llvm::StringRef File) -> const DraftStore::Draft & {
    auto It = llvm::find_if(
        Snapshot, [&](const auto &Entry) { return Entry.first == File; });
    EXPECT_NE(It, Snapshot.end());
    return It->second;
  };
  EXPECT_EQ(Find("a.cpp").Version, "3");
  EXPECT_EQ(*Find("a.cpp").Contents, "old a");
  EXPECT_EQ(Find("b.cpp").Version, "7");
  EXPECT_EQ(*Find("b.cpp").Contents, "old b");
}

} // namespace
} // namespace clangd
} // namespace clang
