//===--- FileRenameOptions.cpp - Validate rename-sensitive options --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CompilerInvocation.h"
#include "FileRename.h"
#include "FileRenameInternal.h"
#include "support/Logger.h"
#include "clang/Options/Options.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Path.h"
#include <functional>
#include <system_error>

namespace clang {
namespace clangd {
namespace {

enum class PathSemantics {
  None,
  Path,
  TreeRoot,
  ModuleFile,
  RemapPair,
  ZOSList,
  IncludePrefix,
  WithPrefix,
  WithSysroot,
  CommandInput,
  HeaderSpelling,
};

PathSemantics pathSemantics(unsigned ID) {
  using namespace options;
  switch (ID) {
  case OPT_I:
  case OPT_F:
  case OPT_embed_dir_EQ:
  case OPT_fmodule_map_file:
  case OPT_fprebuilt_module_path:
  case OPT_fmodules_cache_path:
  case OPT_fmodules_user_build_path:
  case OPT_iquote:
  case OPT_isystem:
  case OPT_isystem_after:
  case OPT_idirafter:
  case OPT_iframework:
  case OPT_iapinotes_modules:
  case OPT_ivfsoverlay:
  case OPT_vfsoverlay:
  case OPT_working_directory:
  case OPT_working_directory_EQ:
  case OPT_c_isystem:
  case OPT_cxx_isystem:
  case OPT_objc_isystem:
  case OPT_objcxx_isystem:
  case OPT_stdlibxx_isystem:
  case OPT_internal_iframework:
  case OPT_internal_isystem:
  case OPT_internal_externc_isystem:
  case OPT__SLASH_imsvc:
  case OPT_fbuild_session_file:
  case OPT_fmodules_embed_file_EQ:
  case OPT_warning_suppression_mappings_EQ:
  case OPT_multi_lib_config:
  case OPT_fplugin_EQ:
  case OPT_fpass_plugin_EQ:
  case OPT_hipspv_pass_plugin_EQ:
  case OPT_hip_device_lib_EQ:
  case OPT_rocm_device_lib_path_EQ:
  case OPT_libomptarget_amdgpu_bc_path_EQ:
  case OPT_libomptarget_nvptx_bc_path_EQ:
  case OPT_libomptarget_spirv_bc_path_EQ:
  case OPT_ast_merge:
  case OPT_foverride_record_layout_EQ:
  case OPT_load:
  case OPT_ccc_gcc_name:
  case OPT__SLASH_Fp:
    return PathSemantics::Path;
  case OPT_B:
  case OPT_gcc_toolchain:
  case OPT_gcc_install_dir_EQ:
  case OPT_ccc_install_dir:
  case OPT__sysroot_EQ:
  case OPT_isysroot:
  case OPT_resource_dir:
  case OPT_resource_dir_EQ:
  case OPT_cuda_path_EQ:
  case OPT_hip_path_EQ:
  case OPT_rocm_path_EQ:
  case OPT_hipstdpar_path_EQ:
  case OPT_hipstdpar_thrust_path_EQ:
  case OPT_hipstdpar_prim_path_EQ:
  case OPT__SLASH_diasdkdir:
  case OPT__SLASH_vctoolsdir:
  case OPT__SLASH_winsdkdir:
  case OPT__SLASH_winsysroot:
  case OPT_config_system_dir_EQ:
  case OPT_config_user_dir_EQ:
    return PathSemantics::TreeRoot;
  case OPT_fmodule_file:
    return PathSemantics::ModuleFile;
  case OPT_remap_file:
    return PathSemantics::RemapPair;
  case OPT_mzos_sys_include_EQ:
    return PathSemantics::ZOSList;
  case OPT_iprefix:
    return PathSemantics::IncludePrefix;
  case OPT_iwithprefix:
  case OPT_iwithprefixbefore:
    return PathSemantics::WithPrefix;
  case OPT_iwithsysroot:
  case OPT_iframeworkwithsysroot:
    return PathSemantics::WithSysroot;
  case OPT_include:
  case OPT_include_pch:
  case OPT_imacros:
  case OPT_chain_include:
    return PathSemantics::CommandInput;
  case OPT_pch_through_header_EQ:
  case OPT__SLASH_Yc:
  case OPT__SLASH_Yu:
    return PathSemantics::HeaderSpelling;
  default:
    return PathSemantics::None;
  }
}

bool isForwardedByResolvedJob(const llvm::opt::Arg &Arg) {
  using namespace options;
  return Arg.getOption().matches(OPT_Xarch__) ||
         Arg.getOption().matches(OPT_Xarch_host) ||
         Arg.getOption().matches(OPT_Xarch_device) ||
         Arg.getOption().matches(OPT_Xopenmp_target) ||
         Arg.getOption().matches(OPT_Xopenmp_target_EQ) ||
         Arg.getOption().matches(OPT_Xoffload_compiler);
}

} // namespace

llvm::Error validateCompileCommandForRenames(
    const tooling::CompileCommand &Command,
    llvm::ArrayRef<std::pair<Path, Path>> Renames,
    llvm::ArrayRef<FileRenameMapping> ExpandedRenames,
    llvm::vfs::FileSystem *FS,
    llvm::ArrayRef<std::string> DriverDerivedCC1Args) {
  assert((FS || ExpandedRenames.empty()) &&
         "expanded renames require a filesystem");

  llvm::SmallVector<Path> CanonicalOldPaths;
  if (FS) {
    CanonicalOldPaths.reserve(Renames.size());
    for (const auto &Rename : Renames) {
      auto Canonical = fileRenameCanonicalPath(Rename.first, *FS);
      if (!Canonical)
        return Canonical.takeError();
      CanonicalOldPaths.push_back(std::move(*Canonical));
    }
  }

  auto CheckPath = [&](llvm::StringRef Value, llvm::StringRef Option,
                       bool IncludesDescendants = false) -> llvm::Error {
    if (Value.empty())
      return error("compiler option {0} has an empty path", Option);
    llvm::SmallString<256> Absolute(Value);
    if (!llvm::sys::path::is_absolute(Absolute)) {
      Absolute = Command.Directory;
      llvm::sys::path::append(Absolute, Value);
    }
    llvm::sys::path::remove_dots(Absolute, /*remove_dot_dot=*/true);
    if (IncludesDescendants)
      for (const auto &Rename : Renames)
        if (fileRenamePathInside(Absolute, Rename.first))
          return error("file rename moves compiler path {0} from option {1}",
                       Absolute, Option);
    auto Mapped = mapPathAfterRenames(Absolute, Renames);
    if (!Mapped)
      return Mapped.takeError();
    if (*Mapped != Absolute)
      return error("file rename moves compiler path {0} from option {1}",
                   Absolute, Option);
    if (!FS)
      return llvm::Error::success();

    auto Canonical = fileRenameCanonicalPath(Absolute, *FS);
    if (!Canonical)
      return Canonical.takeError();
    for (PathRef Old : CanonicalOldPaths)
      if (fileRenamePathInside(Old, *Canonical) ||
          (IncludesDescendants && fileRenamePathInside(*Canonical, Old)))
        return error("file rename moves compiler path {0} from option {1}",
                     Absolute, Option);
    auto S = FS->status(Absolute);
    if (S) {
      if (llvm::any_of(ExpandedRenames, [&](const auto &Rename) {
            return Rename.OldIdentity == S->getUniqueID();
          }))
        return error("file rename moves compiler path {0} from option {1}",
                     Absolute, Option);
    } else if (S.getError() != std::errc::no_such_file_or_directory) {
      return error("cannot inspect compiler path {0} from option {1}: {2}",
                   Absolute, Option, S.getError().message());
    }
    return llvm::Error::success();
  };

  auto CheckHeaderSpelling = [&](llvm::StringRef Value,
                                 llvm::StringRef Option) -> llvm::Error {
    if (Value.empty())
      return llvm::Error::success();
    if (llvm::sys::path::is_absolute(Value) ||
        llvm::sys::path::has_parent_path(Value))
      return CheckPath(Value, Option);
    for (const auto &Rename : Renames)
      if (llvm::sys::path::filename(Rename.first) == Value)
        return error("file rename moves compiler header {0} named by option "
                     "{1}",
                     Rename.first, Option);
    return llvm::Error::success();
  };

  if (auto Err = CheckPath(Command.Directory, "compilation working directory"))
    return Err;
  if (Command.CommandLine.empty())
    return error("compiler command line is empty");
  if (Command.HadResponseFile)
    return error("cannot prove compiler response-file expansion after rename");
  if (Command.HadConfigFile)
    return error("cannot prove compiler configuration-file expansion after "
                 "rename");

  auto Normalized = normalizeCompilerCommand(Command);
  if (!Normalized)
    return Normalized.takeError();

  if (Normalized->Mode != CompilerInvocationMode::DirectCC1) {
    if (compilerLoadsConfigFile(Command))
      return error("compiler driver currently loads a configuration file");
  }

  llvm::StringRef Executable = Command.CommandLine.front();
  if (llvm::sys::path::is_absolute(Executable) ||
      llvm::sys::path::has_parent_path(Executable)) {
    if (auto Err = CheckPath(Executable, "compiler executable"))
      return Err;
  } else {
    for (const auto &Rename : Renames)
      if (llvm::sys::path::filename(Rename.first) == Executable)
        return error("file rename moves compiler executable {0}", Rename.first);
  }

  for (llvm::StringRef Arg : llvm::ArrayRef(Command.CommandLine).drop_front())
    if (Arg.starts_with("@"))
      return error("cannot prove compiler response file after rename: {0}",
                   Arg);

  std::function<llvm::Error(llvm::ArrayRef<llvm::StringRef>,
                            llvm::opt::Visibility, unsigned)>
      ParseAndCheck;
  ParseAndCheck = [&](llvm::ArrayRef<llvm::StringRef> Args,
                      llvm::opt::Visibility Visibility,
                      unsigned Depth) -> llvm::Error {
    if (Depth > 8)
      return error("compiler option forwarding is nested too deeply");
    llvm::SmallVector<const char *> RawArgs;
    RawArgs.reserve(Args.size());
    for (llvm::StringRef Arg : Args)
      RawArgs.push_back(Arg.data());
    unsigned MissingIndex = 0;
    unsigned MissingCount = 0;
    llvm::opt::InputArgList Parsed = getDriverOptTable().ParseArgs(
        RawArgs, MissingIndex, MissingCount, Visibility);
    if (MissingCount)
      return error("compiler option {0} has no argument", Args[MissingIndex]);

    llvm::StringRef Sysroot;
    for (const llvm::opt::Arg *Arg : Parsed)
      if (Arg->getOption().matches(options::OPT_isysroot) ||
          Arg->getOption().matches(options::OPT__sysroot_EQ))
        Sysroot = Arg->getValue();

    llvm::SmallVector<llvm::StringRef> ForwardedDriver;
    llvm::SmallVector<llvm::StringRef> ForwardedCC1;
    llvm::StringRef IncludePrefix;
    for (const llvm::opt::Arg *Arg : Parsed) {
      if (Arg->getOption().matches(options::OPT_UNKNOWN))
        return error("cannot classify compiler option {0}", Arg->getSpelling());
      if (Arg->getOption().matches(options::OPT_config))
        return error("cannot prove compiler configuration file {0} after "
                     "rename",
                     Arg->getValue());
      if (Arg->getOption().matches(options::OPT_Xclang)) {
        llvm::append_range(ForwardedCC1, Arg->getValues());
        continue;
      }
      if (Arg->getOption().matches(options::OPT_Xpreprocessor) ||
          Arg->getOption().matches(options::OPT_Wp_COMMA) ||
          Arg->getOption().matches(options::OPT__SLASH_clang)) {
        llvm::append_range(ForwardedDriver, Arg->getValues());
        continue;
      }
      if (isForwardedByResolvedJob(*Arg)) {
        return error("cannot prove all target-selected compiler jobs from "
                     "option {0}",
                     Arg->getSpelling());
      }

      const unsigned ID = Arg->getOption().getUnaliasedOption().getID();
      PathSemantics Semantics = pathSemantics(ID);
      if (Semantics == PathSemantics::None)
        continue;
      if (Arg->getNumValues() != 1)
        return error("compiler option {0} does not have exactly one path",
                     Arg->getSpelling());
      llvm::StringRef Value = Arg->getValue();
      bool ExpandsEqualsPath =
          ID == options::OPT_I || ID == options::OPT_idirafter ||
          ID == options::OPT_iquote || ID == options::OPT_isystem;
      if (ExpandsEqualsPath && Value.starts_with("=")) {
        if (Sysroot.empty())
          return error("cannot resolve sysroot-relative compiler path {0}",
                       Value);
        llvm::SmallString<256> Expanded(Sysroot);
        llvm::sys::path::append(Expanded, Value.drop_front());
        if (auto Err = CheckPath(Expanded, Arg->getSpelling()))
          return Err;
        continue;
      }
      switch (Semantics) {
      case PathSemantics::None:
        llvm_unreachable("handled above");
      case PathSemantics::Path:
        if (!Value.empty())
          if (auto Err = CheckPath(Value, Arg->getSpelling()))
            return Err;
        break;
      case PathSemantics::TreeRoot: {
        llvm::SmallString<256> Expanded(Value);
        bool SysrootRelativeB = ID == options::OPT_B &&
                                ((Arg->getIndex() < Args.size() &&
                                  Args[Arg->getIndex()].starts_with("-B=")) ||
                                 (Arg->getIndex() + 1 < Args.size() &&
                                  Args[Arg->getIndex()] == "-B" &&
                                  Args[Arg->getIndex() + 1].starts_with("=")));
        if (SysrootRelativeB) {
          if (Sysroot.empty())
            return error("cannot resolve compiler -B prefix {0} without a "
                         "sysroot",
                         Value);
          Expanded = Sysroot;
          llvm::sys::path::append(
              Expanded, Value.starts_with("=") ? Value.drop_front() : Value);
        }
        if (auto Err = CheckPath(Expanded, Arg->getSpelling(),
                                 /*IncludesDescendants=*/true))
          return Err;
        break;
      }
      case PathSemantics::ModuleFile:
        if (size_t Equals = Value.rfind('='); Equals != llvm::StringRef::npos)
          Value = Value.drop_front(Equals + 1);
        if (auto Err = CheckPath(Value, Arg->getSpelling()))
          return Err;
        break;
      case PathSemantics::RemapPair: {
        auto Pair = Value.split(';');
        if (Pair.first.empty() || Pair.second.empty() ||
            Pair.second.contains(';'))
          return error("compiler option {0} has malformed file pair {1}",
                       Arg->getSpelling(), Value);
        if (auto Err = CheckPath(Pair.first, Arg->getSpelling()))
          return Err;
        if (auto Err = CheckPath(Pair.second, Arg->getSpelling()))
          return Err;
        break;
      }
      case PathSemantics::ZOSList:
        while (!Value.empty()) {
          auto Item = Value.split(':');
          if (!Item.first.empty())
            if (auto Err = CheckPath(Item.first, Arg->getSpelling()))
              return Err;
          Value = Item.second;
        }
        break;
      case PathSemantics::IncludePrefix:
        IncludePrefix = Value;
        if (auto Err = CheckPath(Value, Arg->getSpelling()))
          return Err;
        break;
      case PathSemantics::WithPrefix:
        if (auto Err =
                CheckPath((IncludePrefix + Value).str(), Arg->getSpelling()))
          return Err;
        break;
      case PathSemantics::WithSysroot:
        if (!Sysroot.empty() && llvm::sys::path::is_absolute(Value)) {
          if (auto Err = CheckPath((Sysroot + Value).str(), Arg->getSpelling()))
            return Err;
        } else if (auto Err = CheckPath(Value, Arg->getSpelling())) {
          return Err;
        }
        break;
      case PathSemantics::CommandInput:
        if (llvm::sys::path::is_absolute(Value) ||
            llvm::sys::path::has_parent_path(Value))
          if (auto Err = CheckPath(Value, Arg->getSpelling()))
            return Err;
        break;
      case PathSemantics::HeaderSpelling:
        if (auto Err = CheckHeaderSpelling(Value, Arg->getSpelling()))
          return Err;
        break;
      }
    }
    if (!ForwardedDriver.empty())
      if (auto Err = ParseAndCheck(ForwardedDriver,
                                   llvm::opt::Visibility(options::ClangOption),
                                   Depth + 1))
        return Err;
    if (!ForwardedCC1.empty())
      if (auto Err = ParseAndCheck(ForwardedCC1,
                                   llvm::opt::Visibility(options::CC1Option),
                                   Depth + 1))
        return Err;
    return llvm::Error::success();
  };

  llvm::SmallVector<llvm::StringRef> OriginalArgs;
  CompilerInvocationMode Mode = compilerInvocationMode(Command.CommandLine);
  size_t FirstArg = Mode == CompilerInvocationMode::DirectCC1 ? 2 : 1;
  for (llvm::StringRef Arg :
       llvm::ArrayRef(Command.CommandLine).drop_front(FirstArg))
    OriginalArgs.push_back(Arg);
  llvm::opt::Visibility Visibility(
      Mode == CompilerInvocationMode::ClangCL     ? options::CLOption
      : Mode == CompilerInvocationMode::DirectCC1 ? options::CC1Option
                                                  : options::ClangOption);
  if (auto Err = ParseAndCheck(OriginalArgs, Visibility, 0))
    return Err;

  if (!DriverDerivedCC1Args.empty()) {
    llvm::SmallVector<llvm::StringRef> Derived;
    Derived.reserve(DriverDerivedCC1Args.size());
    for (llvm::StringRef Arg : DriverDerivedCC1Args)
      Derived.push_back(Arg);
    if (!Derived.empty() && Derived.front() == "-cc1")
      Derived.erase(Derived.begin());
    if (auto Err = ParseAndCheck(Derived,
                                 llvm::opt::Visibility(options::CC1Option), 0))
      return Err;
  }
  return llvm::Error::success();
}

} // namespace clangd
} // namespace clang
