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
#include "clang/Basic/LangOptions.h"
#include "clang/Lex/DependencyDirectivesScanner.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

namespace clang {
namespace clangd {
namespace {

struct ScannedDependencyDirectives {
  std::unique_ptr<llvm::MemoryBuffer> Buffer;
  llvm::SmallVector<dependency_directives_scan::Token> Tokens;
  llvm::SmallVector<dependency_directives_scan::Directive> Directives;
};

llvm::Expected<std::unique_ptr<ScannedDependencyDirectives>>
scanDependencyDirectives(PathRef File, llvm::vfs::FileSystem &FS) {
  auto Buffer = FS.getBufferForFile(File);
  if (!Buffer)
    return error("cannot read dependency directives from {0}: {1}", File,
                 Buffer.getError().message());
  auto Result = std::make_unique<ScannedDependencyDirectives>();
  Result->Buffer = std::move(*Buffer);
  if (scanSourceForDependencyDirectives(Result->Buffer->getBuffer(),
                                        Result->Tokens,
                                        Result->Directives)) {
    if (llvm::sys::path::filename(File) != "module.map" &&
        llvm::sys::path::extension(File) != ".modulemap")
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
  auto HeaderSpelling = [&](llvm::ArrayRef<dependency_directives_scan::Token>
                                Tokens) -> llvm::StringRef {
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
  bool ModuleMap = llvm::sys::path::filename(File) == "module.map" ||
                   llvm::sys::path::extension(File) == ".modulemap";
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
    if (Token.is(tok::raw_identifier)) {
      llvm::StringRef Name = Token.getRawIdentifier();
      if (Name == "_Pragma") {
        ScanState = State::AfterPragma;
        continue;
      }
      switch (ScanState) {
      case State::AfterHash:
        if (Name == "embed")
          Result.UneditableDependencies.push_back({"#embed directive", {}});
        ScanState = Name == "embed" ? State::EmbedPath
                    : Name == "pragma" ? State::AfterPragma
                                       : State::None;
        break;
      case State::AfterPragma:
        ScanState = (Name == "GCC" || Name == "clang")
                        ? State::AfterPragmaVendor
                        : State::None;
        break;
      case State::AfterPragmaVendor:
        ScanState = Name == "dependency" ? State::PragmaDependencyPath
                                          : State::None;
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
      else if (ScanState == State::AfterPragma &&
               Written.contains("dependency"))
        Result.UneditableDependencies.push_back({"_Pragma dependency", {}});
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
  if (AssemblySource &&
      (Code.contains(".incbin") || Code.contains(".include")))
    Result.UneditableDependencies.push_back({"assembly file directive", {}});

  enum class AsmState { None, AwaitingArguments, Arguments };
  AsmState InlineAsm = AsmState::None;
  unsigned AsmDepth = 0;
  bool InAsmTemplate = false;
  bool SawLiteralTemplate = false;
  bool UnprovenTemplate = false;
  bool HasFileDirective = false;
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
      llvm::StringRef Literal(Token.getLiteralData(), Token.getLength());
      HasFileDirective |=
          Literal.contains(".incbin") || Literal.contains(".include");
    } else {
      UnprovenTemplate = true;
    }
  } while (true);
}

} // namespace

llvm::Expected<FileRenameDirectiveScan>
scanFileRenameDirectives(PathRef File, llvm::vfs::FileSystem &FS) {
  auto Scan = scanDependencyDirectives(File, FS);
  if (!Scan)
    return Scan.takeError();
  const auto &Directives = (*Scan)->Directives;
  llvm::StringRef Code = (*Scan)->Buffer->getBuffer();
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

llvm::Expected<const FileRenameDirectiveScan *>
FileRenameDirectiveCache::scan(PathRef File, FileDigest ExpectedDigest) {
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
  auto Result = scanFileRenameDirectives(File, FS);
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

llvm::Expected<bool> hasIncludeDirectives(PathRef File,
                                          llvm::vfs::FileSystem &FS) {
  auto Scan = scanFileRenameDirectives(File, FS);
  if (!Scan)
    return Scan.takeError();
  return Scan->HasIncludeDirectives;
}

} // namespace clangd
} // namespace clang
