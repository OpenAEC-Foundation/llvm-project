//===-- BackgroundIndexTestUtils.h ------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_UNITTESTS_BACKGROUNDINDEXTESTUTILS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_UNITTESTS_BACKGROUNDINDEXTESTUTILS_H

#include "RIFF.h"
#include "TestFS.h"
#include "index/Background.h"
#include "index/Serialization.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace clang {
namespace clangd {

inline llvm::Expected<std::string> contextCacheWithState(llvm::StringRef Shard,
                                                         bool Malformed) {
  auto Parsed = riff::readFile(Shard);
  if (!Parsed)
    return Parsed.takeError();
  auto Context = llvm::find_if(Parsed->Chunks, [](riff::Chunk Chunk) {
    return Chunk.ID == riff::fourCC("ctxs");
  });
  if (Context == Parsed->Chunks.end())
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "index shard has no context-source chunk");
  if (!Malformed) {
    Parsed->Chunks.erase(Context);
    return llvm::to_string(*Parsed);
  }
  if (Context->Data.size() < 4)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "context-source chunk is too small");
  std::string CorruptContext = Context->Data.str();
  CorruptContext[0] = 2;
  Context->Data = CorruptContext;
  return llvm::to_string(*Parsed);
}

class MemoryShardStorage : public BackgroundIndexStorage {
  mutable std::mutex StorageMu;
  llvm::StringMap<std::string> &Storage;
  size_t &CacheHits;

public:
  MemoryShardStorage(llvm::StringMap<std::string> &Storage, size_t &CacheHits)
      : Storage(Storage), CacheHits(CacheHits) {}
  llvm::Error storeShard(llvm::StringRef ShardIdentifier,
                         IndexFileOut Shard) const override {
    if (BeforeStore)
      BeforeStore(ShardIdentifier);
    std::lock_guard<std::mutex> Lock(StorageMu);
    AccessedPaths.insert(ShardIdentifier);
    StoredPaths.insert(ShardIdentifier);
    ++StoreCounts[ShardIdentifier];
    Storage[ShardIdentifier] = llvm::to_string(Shard);
    return llvm::Error::success();
  }
  llvm::Error removeShard(llvm::StringRef ShardIdentifier) const override {
    std::lock_guard<std::mutex> Lock(StorageMu);
    AccessedPaths.insert(ShardIdentifier);
    Storage.erase(ShardIdentifier);
    return llvm::Error::success();
  }
  llvm::Error clear() const override {
    std::lock_guard<std::mutex> Lock(StorageMu);
    Storage.clear();
    WasCleared = true;
    return llvm::Error::success();
  }
  std::unique_ptr<IndexFileIn>
  loadShard(llvm::StringRef ShardIdentifier) const override {
    if (BeforeLoad)
      BeforeLoad(ShardIdentifier);
    std::lock_guard<std::mutex> Lock(StorageMu);
    AccessedPaths.insert(ShardIdentifier);
    if (!Storage.contains(ShardIdentifier))
      return nullptr;
    auto IndexFile =
        readIndexFile(Storage[ShardIdentifier], SymbolOrigin::Background);
    if (!IndexFile) {
      if (AllowLoadErrors)
        llvm::consumeError(IndexFile.takeError());
      else
        ADD_FAILURE() << "Error while reading " << ShardIdentifier << ':'
                      << IndexFile.takeError();
      return nullptr;
    }
    ++CacheHits;
    return std::make_unique<IndexFileIn>(std::move(*IndexFile));
  }

  mutable llvm::StringSet<> AccessedPaths;
  mutable llvm::StringSet<> StoredPaths;
  mutable llvm::StringMap<unsigned> StoreCounts;
  std::function<void(PathRef)> BeforeStore;
  std::function<void(PathRef)> BeforeLoad;
  bool AllowLoadErrors = false;
  mutable bool WasCleared = false;
};

class CountingMockFS : public MockFS {
  class CountingView : public llvm::vfs::ProxyFileSystem {
  public:
    CountingView(llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> Base,
                 const CountingMockFS &Owner)
        : ProxyFileSystem(std::move(Base)), Owner(Owner) {}

    llvm::ErrorOr<std::unique_ptr<llvm::vfs::File>>
    openFileForRead(const llvm::Twine &Path) override {
      {
        std::lock_guard<std::mutex> Lock(Owner.ReadMu);
        ++Owner.Reads[Path.str()];
      }
      return ProxyFileSystem::openFileForRead(Path);
    }

  private:
    const CountingMockFS &Owner;
  };

public:
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> viewImpl() const override {
    return new CountingView(MockFS::viewImpl(), *this);
  }

  void resetReads() const {
    std::lock_guard<std::mutex> Lock(ReadMu);
    Reads.clear();
  }

  size_t reads(PathRef File) const {
    std::lock_guard<std::mutex> Lock(ReadMu);
    return Reads.lookup(File);
  }

private:
  mutable std::mutex ReadMu;
  mutable llvm::StringMap<size_t> Reads;
};

class BackgroundIndexTest : public ::testing::Test {
protected:
  BackgroundIndexTest() { BackgroundQueue::preventThreadStarvationInTests(); }
};

} // namespace clangd
} // namespace clang

#endif
