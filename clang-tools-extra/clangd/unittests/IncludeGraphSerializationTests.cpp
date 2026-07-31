//===-- IncludeGraphSerializationTests.cpp ----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Headers.h"
#include "RIFF.h"
#include "index/Serialization.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Testing/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <string>
#include <vector>

namespace clang {
namespace clangd {
namespace {

using ::testing::ElementsAre;
using ::testing::UnorderedElementsAre;

TEST(SerializationTest, ContextSourcesRoundTripAndRejectsUnknownSchema) {
  SymbolSlab Symbols;
  IncludeGraph Context;
  IncludeGraphNode Main;
  Main.URI = "file:///workspace/main.cpp";
  Main.Digest = digest("main");
  Main.DirectIncludes = {"file:///workspace/common.h"};
  Main.Flags |= IncludeGraphNode::SourceFlag::IsTU;
  Context[Main.URI] = Main;
  IncludeGraphNode Header;
  Header.URI = "file:///workspace/common.h";
  Header.Digest = digest("header");
  Header.Flags |= IncludeGraphNode::SourceFlag::HasConditionalIncludes;
  Context[Header.URI] = Header;

  IndexFileOut Out;
  Out.Symbols = &Symbols;
  Out.Format = IndexFileFormat::RIFF;
  Out.ContextSources = &Context;
  std::vector<std::string> CC1Command = {"-cc1", "-I", "/workspace/include"};
  Out.CC1CommandLine = &CC1Command;
  std::string Serialized = llvm::to_string(Out);
  auto RoundTrip = readIndexFile(Serialized, SymbolOrigin::Unknown);
  ASSERT_TRUE(bool(RoundTrip)) << RoundTrip.takeError();
  ASSERT_TRUE(RoundTrip->ContextSources);
  EXPECT_THAT(RoundTrip->ContextSources->keys(),
              UnorderedElementsAre(Main.URI, Header.URI));
  EXPECT_THAT(RoundTrip->ContextSources->lookup(Main.URI).DirectIncludes,
              ElementsAre(Header.URI));
  EXPECT_TRUE(RoundTrip->ContextSources->lookup(Header.URI).Flags &
              IncludeGraphNode::SourceFlag::HasConditionalIncludes);
  ASSERT_TRUE(RoundTrip->CC1CommandLine);
  EXPECT_THAT(*RoundTrip->CC1CommandLine,
              testing::ElementsAreArray(CC1Command));

  auto Parsed = riff::readFile(Serialized);
  ASSERT_TRUE(bool(Parsed)) << Parsed.takeError();
  auto ContextChunk = llvm::find_if(Parsed->Chunks, [](riff::Chunk C) {
    return C.ID == riff::fourCC("ctxs");
  });
  ASSERT_NE(ContextChunk, Parsed->Chunks.end());
  ASSERT_GE(ContextChunk->Data.size(), 4U);
  std::string UnknownSchema = ContextChunk->Data.str();
  UnknownSchema[0] = 2;
  ContextChunk->Data = UnknownSchema;
  auto Rejected =
      readIndexFile(llvm::to_string(*Parsed), SymbolOrigin::Unknown);
  ASSERT_FALSE(bool(Rejected));
  EXPECT_THAT(llvm::toString(Rejected.takeError()),
              testing::HasSubstr("context include graph schema"));
}

TEST(SerializationTest, RejectsMalformedContextSources) {
  SymbolSlab Symbols;
  IncludeGraph Context;
  IncludeGraphNode Main;
  Main.URI = "file:///workspace/main.cpp";
  Main.Digest = digest("main");
  Main.Flags = IncludeGraphNode::SourceFlag::IsTU;
  Main.DirectIncludes = {"file:///workspace/missing.h"};
  Context[Main.URI] = Main;

  IndexFileOut Out;
  Out.Symbols = &Symbols;
  Out.Format = IndexFileFormat::RIFF;
  Out.ContextSources = &Context;
  auto Undefined = readIndexFile(llvm::to_string(Out), SymbolOrigin::Unknown);
  ASSERT_FALSE(bool(Undefined));
  EXPECT_THAT(llvm::toString(Undefined.takeError()),
              testing::HasSubstr("undefined"));

  IncludeGraphNode Header;
  Header.URI = "file:///workspace/missing.h";
  Header.Digest = digest("header");
  Context[Header.URI] = Header;
  std::string Serialized = llvm::to_string(Out);
  auto Parsed = riff::readFile(Serialized);
  ASSERT_TRUE(bool(Parsed)) << Parsed.takeError();
  auto ContextChunk = llvm::find_if(Parsed->Chunks, [](riff::Chunk C) {
    return C.ID == riff::fourCC("ctxs");
  });
  ASSERT_NE(ContextChunk, Parsed->Chunks.end());
  std::string Duplicate = ContextChunk->Data.str();
  Duplicate += ContextChunk->Data.drop_front(4).str();
  ContextChunk->Data = Duplicate;
  auto Duplicated =
      readIndexFile(llvm::to_string(*Parsed), SymbolOrigin::Unknown);
  ASSERT_FALSE(bool(Duplicated));
  EXPECT_THAT(llvm::toString(Duplicated.takeError()),
              testing::HasSubstr("duplicate node"));

  Context[Header.URI].Flags = static_cast<IncludeGraphNode::SourceFlag>(0x80);
  EXPECT_THAT_ERROR(
      validateContextIncludeGraph(Context),
      llvm::FailedWithMessage(testing::HasSubstr("unknown flags")));
  Context[Header.URI].Flags = IncludeGraphNode::SourceFlag::None;
  Context[Main.URI].DirectIncludes.clear();
  EXPECT_THAT_ERROR(
      validateContextIncludeGraph(Context),
      llvm::FailedWithMessage(testing::HasSubstr("unreachable node")));
  Context[Header.URI].Flags = IncludeGraphNode::SourceFlag::IsTU;
  EXPECT_THAT_ERROR(
      validateContextIncludeGraph(Context),
      llvm::FailedWithMessage(testing::HasSubstr("multiple translation")));
  Context[Header.URI].Flags = IncludeGraphNode::SourceFlag::IsCommandInput;
  Context[Header.URI].Digest = FileDigest{{0}};
  EXPECT_THAT_ERROR(
      validateContextIncludeGraph(Context),
      llvm::FailedWithMessage(testing::HasSubstr("invalid digest")));
  Context[Header.URI].Digest = digest("header");
  EXPECT_THAT_ERROR(
      validateContextIncludeGraph(Context, "file:///workspace/other.cpp"),
      llvm::FailedWithMessage(testing::HasSubstr("expected")));
  FileDigest WrongDigest = digest("different");
  EXPECT_THAT_ERROR(
      validateContextIncludeGraph(Context, Main.URI, &WrongDigest),
      llvm::FailedWithMessage(testing::HasSubstr("main-file digest")));
}

TEST(SerializationTest, DropsUnknownRIFFChunksOnRewrite) {
  SymbolSlab Symbols;
  IndexFileOut Out;
  Out.Symbols = &Symbols;
  Out.Format = IndexFileFormat::RIFF;
  std::string Serialized = llvm::to_string(Out);
  auto Parsed = riff::readFile(Serialized);
  ASSERT_TRUE(bool(Parsed)) << Parsed.takeError();
  Parsed->Chunks.push_back({riff::fourCC("junk"), "ignored"});
  auto WithUnknown =
      readIndexFile(llvm::to_string(*Parsed), SymbolOrigin::Unknown);
  ASSERT_TRUE(bool(WithUnknown)) << WithUnknown.takeError();
  auto Rewritten = riff::readFile(llvm::to_string(IndexFileOut(*WithUnknown)));
  ASSERT_TRUE(bool(Rewritten)) << Rewritten.takeError();
  EXPECT_TRUE(llvm::none_of(Rewritten->Chunks, [](riff::Chunk C) {
    return C.ID == riff::fourCC("junk");
  }));
}

} // namespace
} // namespace clangd
} // namespace clang
