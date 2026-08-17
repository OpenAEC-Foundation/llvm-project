//===--- FileRenameDirectives.cpp - Rename dependency scans -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FileRename.h"
#include "FileRenameInternal.h"
#include "SourceCode.h"
#include "support/Logger.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Lex/DependencyDirectivesScanner.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/LiteralSupport.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/TargetParser/Host.h"
#include <cstdint>
#include <limits>
#include <memory>

namespace clang {
namespace clangd {
namespace {

struct ScannedDependencyDirectives {
  std::unique_ptr<llvm::MemoryBuffer> Buffer;
  llvm::SmallVector<dependency_directives_scan::Token> Tokens;
  llvm::SmallVector<dependency_directives_scan::Directive> Directives;
};

llvm::Expected<std::string>
decodeStringLiteralTokens(llvm::ArrayRef<Token> Tokens, bool Trigraphs) {
  auto DiagIDs = llvm::makeIntrusiveRefCnt<DiagnosticIDs>();
  DiagnosticOptions DiagOpts;
  DiagnosticsEngine Diags(DiagIDs, DiagOpts, new IgnoringDiagConsumer(),
                          /*ShouldOwnClient=*/true);
  FileSystemOptions FSOpts;
  FileManager Files(FSOpts);
  SourceManager Sources(Diags, Files);
  TargetOptions TargetOpts;
  TargetOpts.Triple = llvm::sys::getDefaultTargetTriple();
  auto Target = TargetInfo::CreateTargetInfo(Diags, TargetOpts);
  if (!Target)
    return error("cannot create target for string-literal decoding");
  LangOptions LangOpts;
  LangOpts.CPlusPlus = true;
  LangOpts.Trigraphs = Trigraphs;
  StringLiteralParser Parser(Tokens, Sources, LangOpts, *Target);
  if (Parser.hadError || !Parser.isOrdinary())
    return error("cannot decode inline assembly string-literal sequence");
  return Parser.GetString().str();
}

llvm::Expected<std::unique_ptr<ScannedDependencyDirectives>>
scanDependencyDirectives(PathRef File, llvm::vfs::FileSystem &FS,
                         uint64_t MaxBytes) {
  auto Initial = FS.status(File);
  if (!Initial)
    return error("cannot inspect dependency directives in {0}: {1}", File,
                 Initial.getError().message());
  if (!Initial->isRegularFile())
    return error("dependency directive input is not a regular file: {0}", File);
  if (Initial->getSize() > MaxBytes)
    return error("dependency directive input {0} is {1} bytes, exceeding the "
                 "{2}-byte scan limit",
                 File, Initial->getSize(), MaxBytes);
  if (Initial->getSize() >
      static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
    return error("dependency directive input is too large to read: {0}", File);
  auto Buffer =
      FS.getBufferForFile(File, static_cast<int64_t>(Initial->getSize()),
                          /*RequiresNullTerminator=*/true, /*IsVolatile=*/true,
                          /*IsText=*/false);
  if (!Buffer)
    return error("cannot read dependency directives from {0}: {1}", File,
                 Buffer.getError().message());
  auto Current = FS.status(File);
  if (!Current || !Current->isRegularFile() ||
      Current->getUniqueID() != Initial->getUniqueID() ||
      Current->getLastModificationTime() !=
          Initial->getLastModificationTime() ||
      Current->getSize() != Initial->getSize() ||
      Buffer.get()->getBufferSize() != Initial->getSize())
    return error("dependency directive input changed while being read: {0}",
                 File);
  auto Result = std::make_unique<ScannedDependencyDirectives>();
  Result->Buffer = std::move(*Buffer);
  if (scanSourceForDependencyDirectives(Result->Buffer->getBuffer(),
                                        Result->Tokens, Result->Directives)) {
    if (!pathEqual(llvm::sys::path::filename(File), "module.map") &&
        !pathEqual(llvm::sys::path::extension(File), ".modulemap"))
      return error("cannot scan dependency directives in {0}", File);
    // Module-map syntax is not preprocessor syntax. Its file dependencies are
    // handled by the raw lexer below, and partial scanner output is unusable.
    Result->Tokens.clear();
    Result->Directives.clear();
  }
  return Result;
}

void scanUneditablePreprocessorQueries(
    llvm::ArrayRef<dependency_directives_scan::Directive> Directives,
    llvm::StringRef Code, FileRenameDirectiveScan &Result) {
  auto HeaderSpelling =
      [&](llvm::ArrayRef<dependency_directives_scan::Token> Tokens)
      -> llvm::StringRef {
    for (const auto &Token : Tokens)
      if (Token.Kind == tok::header_name)
        return Code.slice(Token.Offset, Token.getEnd());
    return {};
  };
  for (const auto &Directive : Directives) {
    using namespace dependency_directives_scan;
    if (Directive.Kind == cxx_import_decl ||
        Directive.Kind == cxx_export_import_decl) {
      llvm::StringRef Written = HeaderSpelling(Directive.Tokens);
      Result.UneditableDependencies.push_back(
          {"C++ header-unit import", Written.str()});
    }
    for (size_t I = 0; I < Directive.Tokens.size(); ++I) {
      const auto &Token = Directive.Tokens[I];
      if (Token.Kind != tok::raw_identifier)
        continue;
      llvm::StringRef Name = Code.slice(Token.Offset, Token.getEnd());
      if (Name != "__has_include" && Name != "__has_include_next" &&
          Name != "__has_embed")
        continue;
      size_t Operand = I + 1;
      if (Operand < Directive.Tokens.size() &&
          Directive.Tokens[Operand].Kind == tok::l_paren)
        ++Operand;
      llvm::StringRef Written;
      if (Operand < Directive.Tokens.size() &&
          Directive.Tokens[Operand].Kind == tok::header_name)
        Written = Code.slice(Directive.Tokens[Operand].Offset,
                             Directive.Tokens[Operand].getEnd());
      Result.UneditableDependencies.push_back({Name.str(), Written.str()});
    }
  }
}

void scanRawDependencies(PathRef File, llvm::StringRef Code,
                         FileRenameDirectiveScan &Result) {
  enum class State {
    None,
    AfterHash,
    AfterPragma,
    AfterPragmaVendor,
    EmbedPath,
    PragmaDependencyPath,
  };
  State ScanState = State::None;
  bool ModuleMap = pathEqual(llvm::sys::path::filename(File), "module.map") ||
                   pathEqual(llvm::sys::path::extension(File), ".modulemap");
  bool ModuleHeaderPath = false;
  bool ModuleUmbrellaPath = false;
  bool ModuleExternPath = false;
  LangOptions LangOpts;
  Lexer RawLexer(SourceLocation(), LangOpts, Code.begin(), Code.begin(),
                 Code.end());
  Token Token;
  do {
    RawLexer.LexFromRawLexer(Token);
    if (Token.is(tok::eof))
      break;
    if (Token.isAtStartOfLine())
      ScanState = State::None;
    if (Token.is(tok::hash) && Token.isAtStartOfLine()) {
      ScanState = State::AfterHash;
      continue;
    }
    if (Token.is(tok::hashhash)) {
      Result.UneditableDependencies.push_back(
          {"macro token-pasting construct", {}});
      continue;
    }
    if (Token.is(tok::raw_identifier)) {
      llvm::StringRef Name = Token.getRawIdentifier();
      if (Name == "_Pragma" || Name == "__pragma") {
        // The operand may itself be produced by macro expansion. The raw
        // dependency scan cannot prove the pragma text in that case.
        Result.UneditableDependencies.push_back({"_Pragma dependency", {}});
        ScanState = State::None;
        continue;
      }
      switch (ScanState) {
      case State::AfterHash:
        if (Name == "embed")
          Result.UneditableDependencies.push_back({"#embed directive", {}});
        ScanState = Name == "embed"    ? State::EmbedPath
                    : Name == "pragma" ? State::AfterPragma
                                       : State::None;
        break;
      case State::AfterPragma:
        ScanState = (Name == "GCC" || Name == "clang")
                        ? State::AfterPragmaVendor
                        : State::None;
        break;
      case State::AfterPragmaVendor:
        ScanState =
            Name == "dependency" ? State::PragmaDependencyPath : State::None;
        break;
      default:
        break;
      }
      if (ModuleMap) {
        ModuleHeaderPath = Name == "header";
        ModuleUmbrellaPath = Name == "umbrella";
        ModuleExternPath |= Name == "extern";
      }
      continue;
    }
    if (Token.is(tok::string_literal)) {
      llvm::StringRef Written(Token.getLiteralData(), Token.getLength());
      if (ScanState == State::EmbedPath)
        Result.UneditableDependencies.push_back(
            {"#embed directive", Written.str()});
      else if (ScanState == State::PragmaDependencyPath)
        Result.UneditableDependencies.push_back(
            {"dependency pragma", Written.str()});
      if (ModuleMap &&
          (ModuleHeaderPath || ModuleUmbrellaPath || ModuleExternPath))
        Result.UneditableDependencies.push_back(
            {"module-map dependency", Written.str(),
             ModuleUmbrellaPath && !ModuleHeaderPath});
    }
    if (ScanState == State::EmbedPath ||
        ScanState == State::PragmaDependencyPath)
      ScanState = State::None;
    ModuleHeaderPath = false;
    ModuleUmbrellaPath = false;
    ModuleExternPath = false;
  } while (true);

  llvm::StringRef Extension = llvm::sys::path::extension(File);
  bool AssemblySource = Extension.equals_insensitive(".s") ||
                        Extension.equals_insensitive(".asm");
  if (AssemblySource && (Code.contains(".incbin") || Code.contains(".include")))
    Result.UneditableDependencies.push_back({"assembly file directive", {}});

  enum class AsmState { None, AwaitingArguments, Arguments };
  AsmState InlineAsm = AsmState::None;
  unsigned AsmDepth = 0;
  bool InAsmTemplate = false;
  bool SawLiteralTemplate = false;
  bool UnprovenTemplate = false;
  bool HasFileDirective = false;
  llvm::SmallVector<clang::Token> AsmLiterals;
  Lexer AsmLexer(SourceLocation(), LangOpts, Code.begin(), Code.begin(),
                 Code.end());
  do {
    AsmLexer.LexFromRawLexer(Token);
    if (Token.is(tok::eof))
      break;
    if (InlineAsm == AsmState::None && Token.is(tok::raw_identifier) &&
        (Token.getRawIdentifier() == "asm" ||
         Token.getRawIdentifier() == "__asm" ||
         Token.getRawIdentifier() == "__asm__")) {
      InlineAsm = AsmState::AwaitingArguments;
      AsmDepth = 0;
      InAsmTemplate = false;
      SawLiteralTemplate = false;
      UnprovenTemplate = false;
      HasFileDirective = false;
      AsmLiterals.clear();
      continue;
    }
    if (InlineAsm == AsmState::AwaitingArguments) {
      if (Token.is(tok::l_paren)) {
        InlineAsm = AsmState::Arguments;
        AsmDepth = 1;
        InAsmTemplate = true;
      } else if (!(Token.is(tok::raw_identifier) &&
                   (Token.getRawIdentifier() == "volatile" ||
                    Token.getRawIdentifier() == "goto"))) {
        InlineAsm = AsmState::None;
      }
      continue;
    }
    if (InlineAsm != AsmState::Arguments)
      continue;
    if (Token.is(tok::l_paren)) {
      ++AsmDepth;
      if (InAsmTemplate)
        UnprovenTemplate = true;
      continue;
    }
    if (Token.is(tok::r_paren)) {
      if (--AsmDepth == 0) {
        if (!UnprovenTemplate && SawLiteralTemplate) {
          auto Literal = decodeStringLiteralTokens(AsmLiterals, false);
          auto TrigraphLiteral = decodeStringLiteralTokens(AsmLiterals, true);
          if (!Literal || !TrigraphLiteral) {
            if (!Literal)
              llvm::consumeError(Literal.takeError());
            if (!TrigraphLiteral)
              llvm::consumeError(TrigraphLiteral.takeError());
            UnprovenTemplate = true;
          } else {
            HasFileDirective =
                llvm::StringRef(*Literal).contains(".incbin") ||
                llvm::StringRef(*Literal).contains(".include") ||
                llvm::StringRef(*TrigraphLiteral).contains(".incbin") ||
                llvm::StringRef(*TrigraphLiteral).contains(".include");
          }
        }
        if (HasFileDirective || UnprovenTemplate || !SawLiteralTemplate)
          Result.UneditableDependencies.push_back(
              {"unproven inline assembly dependency", {}});
        InlineAsm = AsmState::None;
      }
      continue;
    }
    if (AsmDepth != 1 || !InAsmTemplate)
      continue;
    if (Token.is(tok::colon)) {
      InAsmTemplate = false;
      continue;
    }
    if (Token.is(tok::string_literal)) {
      SawLiteralTemplate = true;
      AsmLiterals.push_back(Token);
    } else {
      UnprovenTemplate = true;
    }
  } while (true);
}

llvm::Expected<FileRenameDirectiveScan>
buildDirectiveScan(PathRef File,
                   std::unique_ptr<ScannedDependencyDirectives> Scan) {
  const auto &Directives = Scan->Directives;
  llvm::StringRef Code = Scan->Buffer->getBuffer();
  FileRenameDirectiveScan Result;
  Result.Contents = Code.str();
  Result.Digest = digest(Code);
  Result.HasIncludeDirectives =
      llvm::any_of(Directives, [](const auto &Directive) {
        using namespace dependency_directives_scan;
        return Directive.Kind == pp_include ||
               Directive.Kind == pp_include_next || Directive.Kind == pp_import;
      });
  Result.HasIncludeAliasPragma =
      llvm::any_of(Directives, [](const auto &Directive) {
        return Directive.Kind ==
               dependency_directives_scan::pp_pragma_include_alias;
      });
  for (const auto &Directive : Directives) {
    using namespace dependency_directives_scan;
    if (Directive.Kind != pp_include && Directive.Kind != pp_include_next &&
        Directive.Kind != pp_import)
      continue;
    std::string Written;
    for (const auto &Token : Directive.Tokens)
      if (Token.Kind == tok::header_name) {
        Written = Code.slice(Token.Offset, Token.getEnd()).str();
        break;
      }
    Result.IncludeSpellings.push_back(std::move(Written));
  }
  scanUneditablePreprocessorQueries(Directives, Code, Result);
  scanRawDependencies(File, Code, Result);

  unsigned ConditionalDepth = 0;
  for (const auto &Directive : Directives) {
    using namespace dependency_directives_scan;
    switch (Directive.Kind) {
    case pp_if:
    case pp_ifdef:
    case pp_ifndef:
      ++ConditionalDepth;
      break;
    case pp_endif:
      if (ConditionalDepth == 0)
        return error("unbalanced preprocessor conditional in {0}", File);
      --ConditionalDepth;
      break;
    case pp_elif:
    case pp_elifdef:
    case pp_elifndef:
    case pp_else:
      break;
    case pp_include:
    case pp_include_next:
    case pp_import:
      if (ConditionalDepth > 0) {
        ConditionalInclusion Inclusion;
        Inclusion.Directive = Directive.Kind == pp_include ? tok::pp_include
                              : Directive.Kind == pp_include_next
                                  ? tok::pp_include_next
                                  : tok::pp_import;
        for (const auto &Token : Directive.Tokens) {
          if (Token.Kind == tok::hash)
            Inclusion.HashOffset = Token.Offset;
          if (Token.Kind == tok::header_name)
            Inclusion.Written = Code.slice(Token.Offset, Token.getEnd()).str();
        }
        Inclusion.HashLine = offsetToPosition(Code, Inclusion.HashOffset).line;
        Result.ConditionalIncludes.push_back(std::move(Inclusion));
      }
      break;
    default:
      break;
    }
  }
  if (ConditionalDepth != 0)
    return error("unbalanced preprocessor conditional in {0}", File);
  return Result;
}

} // namespace

llvm::Expected<FileRenameDirectiveScan>
scanFileRenameDirectives(PathRef File, llvm::vfs::FileSystem &FS) {
  auto Scan =
      scanDependencyDirectives(File, FS, std::numeric_limits<uint64_t>::max());
  if (!Scan)
    return Scan.takeError();
  return buildDirectiveScan(File, std::move(*Scan));
}

llvm::Expected<const FileRenameDirectiveScan *>
FileRenameDirectiveCache::scan(PathRef File, FileDigest ExpectedDigest,
                               uint64_t MaxBytes) {
  auto Canonical = fileRenameCanonicalPath(File, FS);
  if (!Canonical)
    return Canonical.takeError();
  std::string Key = maybeCaseFoldPath(*Canonical);
  auto Existing = Scans.find(Key);
  if (Existing != Scans.end()) {
    if (Existing->getValue().Digest != ExpectedDigest)
      return error("canonical workspace aliases have different contents: {0}",
                   File);
    return &Existing->getValue().Scan;
  }
  auto Scan = scanDependencyDirectives(File, FS, MaxBytes);
  if (!Scan)
    return Scan.takeError();
  auto Result = buildDirectiveScan(File, std::move(*Scan));
  if (!Result)
    return Result.takeError();
  if (Result->Digest != ExpectedDigest)
    return error("workspace contents do not match the frozen digest for {0}",
                 File);
  auto Inserted =
      Scans.try_emplace(Key, CachedScan{ExpectedDigest, std::move(*Result)});
  return &Inserted.first->getValue().Scan;
}

llvm::Expected<std::vector<ConditionalInclusion>>
conditionalIncludeDirectives(PathRef File, llvm::vfs::FileSystem &FS) {
  auto Scan = scanFileRenameDirectives(File, FS);
  if (!Scan)
    return Scan.takeError();
  return std::move(Scan->ConditionalIncludes);
}

} // namespace clangd
} // namespace clang
