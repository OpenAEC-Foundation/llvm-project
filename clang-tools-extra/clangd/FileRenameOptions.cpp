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
#include "clang/Driver/ToolChain.h"
#include "clang/Options/Options.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/TargetParser/Triple.h"
#include <functional>
#include <system_error>

namespace clang {
namespace clangd {
namespace {

enum class PathSemantics {
  None,
  Path,
  TreeRoot,
  RemapPair,
  ZOSList,
  IncludePrefix,
  WithPrefix,
  WithSysroot,
  CommandInput,
  HeaderSpelling,
  PathList,
  ImplicitProfile,
};

PathSemantics pathSemantics(unsigned ID) {
  using namespace options;
  switch (ID) {
  case OPT_I:
  case OPT_F:
  case OPT_embed_dir_EQ:
  case OPT_fmodules_cache_path:
  case OPT_fmodules_user_build_path:
  case OPT_iquote:
  case OPT_isystem:
  case OPT_isystem_after:
  case OPT_idirafter:
  case OPT_iframework:
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
  case OPT_foverride_record_layout_EQ:
  case OPT_ccc_gcc_name:
  case OPT_fembed_offload_object_EQ:
  case OPT_fprofile_sample_use_EQ:
  case OPT_fprofile_instr_use_EQ:
  case OPT_fprofile_remapping_file_EQ:
  case OPT_fprofile_list_EQ:
  case OPT_fcodegen_data_use_EQ:
  case OPT_fsanitize_ignorelist_EQ:
  case OPT_fsanitize_system_ignorelist_EQ:
  case OPT_fsanitize_coverage_allowlist:
  case OPT_fsanitize_coverage_ignorelist:
  case OPT_fexperimental_sanitize_metadata_ignorelist_EQ:
  case OPT_frandomize_layout_seed_file_EQ:
  case OPT_fxray_always_instrument:
  case OPT_fxray_never_instrument:
  case OPT_fxray_attr_list:
  case OPT_fms_secure_hotpatch_functions_file:
  case OPT_fthinlto_index_EQ:
  case OPT_mlink_builtin_bitcode:
  case OPT_mlink_bitcode_file:
  case OPT_fprofile_instrument_use_path_EQ:
  case OPT_fcuda_include_gpubinary:
  case OPT_fopenmp_host_ir_file_path:
  case OPT__ssaf_global_scope_analysis_result:
    return PathSemantics::Path;
  case OPT_extract_api_ignores_EQ:
    return PathSemantics::PathList;
  case OPT_fprofile_instr_use:
    return PathSemantics::ImplicitProfile;
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
  case OPT_fprofile_use_EQ:
  case OPT_fmemory_profile_use_EQ:
  case OPT_iapinotes_modules:
    return PathSemantics::TreeRoot;
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
  case OPT_imacros:
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

bool isPluginOption(unsigned ID) {
  using namespace options;
  switch (ID) {
  case OPT_fplugin_EQ:
  case OPT_fplugin_arg:
  case OPT_fpass_plugin_EQ:
  case OPT_hipspv_pass_plugin_EQ:
  case OPT_load:
  case OPT_plugin:
  case OPT_plugin_arg:
  case OPT_add_plugin:
    return true;
  default:
    return false;
  }
}

bool isPrecompiledInputOption(unsigned ID) {
  using namespace options;
  switch (ID) {
  case OPT_include_pch:
  case OPT_fmodule_file:
  case OPT_fprebuilt_module_path:
  case OPT_ast_merge:
  case OPT_chain_include:
  case OPT__SLASH_Fp:
  case OPT__SLASH_Yu:
  case OPT_fmodule_map_file:
  case OPT_ivfsoverlay:
  case OPT_vfsoverlay:
    return true;
  default:
    return false;
  }
}

bool isOpaqueForwardingOption(unsigned ID) {
  using namespace options;
  switch (ID) {
  case OPT_mllvm:
  case OPT_mmlir:
  case OPT_Xassembler:
  case OPT_Xanalyzer:
  case OPT_Wa_COMMA:
  case OPT_Xclangas:
  case OPT_Xcuda_fatbinary:
  case OPT_Xcuda_ptxas:
  case OPT_Xlinker:
  case OPT_Wl_COMMA:
  case OPT_Xthinlto_distributor_EQ:
  case OPT_Xoffload_compiler:
  case OPT_Xoffload_linker:
  case OPT_T:
  case OPT_hip_device_lib_EQ:
  case OPT_rocm_device_lib_path_EQ:
  case OPT_libomptarget_amdgpu_bc_path_EQ:
  case OPT_libomptarget_nvptx_bc_path_EQ:
  case OPT_libomptarget_spirv_bc_path_EQ:
  case OPT_fuse_ld_EQ:
  case OPT_ld_path_EQ:
    return true;
  default:
    return false;
  }
}

bool isNormalHeaderSearchOption(unsigned ID) {
  using namespace options;
  switch (ID) {
  case OPT_I:
  case OPT_iquote:
  case OPT_isystem:
  case OPT_isystem_after:
  case OPT_idirafter:
  case OPT_c_isystem:
  case OPT_cxx_isystem:
  case OPT_objc_isystem:
  case OPT_objcxx_isystem:
  case OPT_stdlibxx_isystem:
  case OPT_internal_isystem:
  case OPT_internal_externc_isystem:
    return true;
  default:
    return false;
  }
}

bool isClangDriverName(llvm::StringRef Executable) {
  driver::ParsedClangName Parsed =
      driver::ToolChain::getTargetAndModeFromProgramName(Executable);
  if (Parsed.isEmpty() || (!Parsed.TargetPrefix.empty() &&
                           llvm::Triple(Parsed.TargetPrefix).getArch() ==
                               llvm::Triple::UnknownArch))
    return false;
  return llvm::is_contained(
      llvm::ArrayRef<llvm::StringLiteral>{"clang", "clang++", "clang-c++",
                                          "clang-cc", "clang-cpp", "clang-g++",
                                          "clang-gcc", "clang-cl"},
      Parsed.ModeSuffix);
}

bool hasCompileOnlyAction(const tooling::CompileCommand &Command,
                          CompilerInvocationMode Mode) {
  unsigned FirstArg = Mode == CompilerInvocationMode::DirectCC1 ? 2 : 1;
  if (Command.CommandLine.size() < FirstArg)
    return false;
  llvm::SmallVector<const char *> Raw;
  for (llvm::StringRef Arg :
       llvm::ArrayRef(Command.CommandLine).drop_front(FirstArg))
    Raw.push_back(Arg.data());
  unsigned MissingIndex = 0;
  unsigned MissingCount = 0;
  auto Parsed = getDriverOptTable().ParseArgs(
      Raw, MissingIndex, MissingCount,
      llvm::opt::Visibility(
          Mode == CompilerInvocationMode::ClangCL     ? options::CLOption
          : Mode == CompilerInvocationMode::DirectCC1 ? options::CC1Option
                                                      : options::ClangOption));
  return MissingCount == 0 &&
         Parsed.hasArg(options::OPT_c, options::OPT_S, options::OPT_E,
                       options::OPT_M, options::OPT_MM,
                       options::OPT_fsyntax_only, options::OPT__SLASH_Zs,
                       options::OPT__SLASH_EP, options::OPT__SLASH_P);
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

  if (Command.CommandLine.empty())
    return error("compiler command line is empty");
  if (Command.HadResponseFile)
    return error("cannot prove compiler response-file expansion after rename");
  if (Command.HadConfigFile)
    return error("cannot prove compiler configuration-file expansion after "
                 "rename");
  if (!llvm::sys::path::is_absolute(Command.Directory))
    return error("compilation working directory is not absolute: {0}",
                 Command.Directory);

  auto Normalized = normalizeCompilerCommand(Command);
  if (!Normalized)
    return Normalized.takeError();
  if (!hasCompileOnlyAction(Command, Normalized->Mode))
    return error("file rename requires a compile-only command");

  llvm::SmallVector<Path> CanonicalOldPaths;
  llvm::SmallVector<Path> CanonicalNewPaths;
  if (FS) {
    CanonicalOldPaths.reserve(Renames.size());
    CanonicalNewPaths.reserve(Renames.size() + ExpandedRenames.size());
    for (const auto &Rename : Renames) {
      auto Canonical = fileRenameCanonicalPath(Rename.first, *FS);
      if (!Canonical)
        return Canonical.takeError();
      CanonicalOldPaths.push_back(std::move(*Canonical));
      Canonical = fileRenameCanonicalPath(Rename.second, *FS);
      if (!Canonical)
        return Canonical.takeError();
      CanonicalNewPaths.push_back(std::move(*Canonical));
    }
    for (const auto &Rename : ExpandedRenames) {
      auto Canonical = fileRenameCanonicalPath(Rename.NewPath, *FS);
      if (!Canonical)
        return Canonical.takeError();
      CanonicalNewPaths.push_back(std::move(*Canonical));
    }
  }

  auto CheckPathFrom = [&](llvm::StringRef Value, PathRef BaseDirectory,
                           llvm::StringRef Option,
                           bool IncludesDescendants = false) -> llvm::Error {
    if (Value.empty())
      return error("compiler option {0} has an empty path", Option);
    llvm::SmallString<256> Absolute(Value);
    if (!llvm::sys::path::is_absolute(Absolute)) {
      Absolute = BaseDirectory;
      llvm::sys::path::append(Absolute, Value);
    }
    llvm::sys::path::remove_dots(Absolute, /*remove_dot_dot=*/true);
    if (IncludesDescendants)
      for (const auto &Rename : Renames)
        if (pathStartsWith(Absolute, Rename.first) ||
            pathStartsWith(Absolute, Rename.second))
          return error("file rename moves compiler path {0} from option {1}",
                       Absolute, Option);
    auto Mapped = mapPathAfterRenames(Absolute, Renames);
    if (!Mapped)
      return Mapped.takeError();
    if (!pathEqual(*Mapped, Absolute))
      return error("file rename moves compiler path {0} from option {1}",
                   Absolute, Option);
    if (!FS)
      return llvm::Error::success();

    auto Canonical = fileRenameCanonicalPath(Absolute, *FS);
    if (!Canonical)
      return Canonical.takeError();
    for (PathRef Old : CanonicalOldPaths)
      if (pathStartsWith(Old, *Canonical) ||
          (IncludesDescendants && pathStartsWith(*Canonical, Old)))
        return error("file rename moves compiler path {0} from option {1}",
                     Absolute, Option);
    for (PathRef New : CanonicalNewPaths)
      if (pathEqual(*Canonical, New) ||
          (IncludesDescendants && (pathStartsWith(*Canonical, New) ||
                                   pathStartsWith(New, *Canonical))))
        return error("file rename changes compiler path namespace {0} from "
                     "option {1}",
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

  auto CheckPath = [&](llvm::StringRef Value, llvm::StringRef Option,
                       bool IncludesDescendants = false) -> llvm::Error {
    return CheckPathFrom(Value, Normalized->EffectiveDirectory, Option,
                         IncludesDescendants);
  };

  auto CheckHeaderSpelling = [&](llvm::StringRef Value,
                                 llvm::StringRef Option) -> llvm::Error {
    if (Value.empty())
      return llvm::Error::success();
    if (llvm::sys::path::is_absolute(Value) ||
        llvm::sys::path::has_parent_path(Value))
      return CheckPath(Value, Option);
    for (const auto &Rename : Renames)
      if (pathEqual(llvm::sys::path::filename(Rename.first), Value) ||
          pathEqual(llvm::sys::path::filename(Rename.second), Value))
        return error("file rename may change compiler header {0} named by "
                     "option {1}",
                     Value, Option);
    return llvm::Error::success();
  };

  if (auto Err = CheckPath(Command.Directory, "compilation working directory"))
    return Err;
  if (Normalized->WorkingDirectory)
    if (auto Err = CheckPath(Normalized->EffectiveDirectory,
                             "compiler working directory"))
      return Err;

  if (Normalized->Mode != CompilerInvocationMode::DirectCC1) {
    if (compilerLoadsConfigFile(Command))
      return error("compiler driver currently loads a configuration file");
    auto JobCount = compilerDriverJobCount(Command);
    if (!JobCount)
      return JobCount.takeError();
    if (*JobCount != 1)
      return error("cannot prove all {0} compiler jobs selected by command",
                   *JobCount);
  }

  llvm::StringRef Executable = Command.CommandLine.front();
  std::string ResolvedExecutable;
  const bool PathResolved = !llvm::sys::path::is_absolute(Executable) &&
                            !llvm::sys::path::has_parent_path(Executable);
  if (PathResolved) {
    auto Found = llvm::sys::findProgramByName(Executable);
    if (!Found)
      return error("cannot resolve compiler executable {0}: {1}", Executable,
                   Found.getError().message());
    ResolvedExecutable = std::move(*Found);
    Executable = ResolvedExecutable;
    llvm::StringRef ExecutableName =
        llvm::sys::path::filename(Command.CommandLine.front());
    auto MayShadow = [&](PathRef Destination) {
      llvm::StringRef DestinationName = llvm::sys::path::filename(Destination);
      return pathEqual(DestinationName, ExecutableName) ||
             pathEqual(DestinationName,
                       llvm::sys::path::filename(ResolvedExecutable));
    };
    for (const auto &Rename : Renames)
      if (MayShadow(Rename.second))
        return error("file rename may change compiler executable resolution at "
                     "{0}",
                     Rename.second);
    for (const auto &Rename : ExpandedRenames)
      if (MayShadow(Rename.NewPath))
        return error("file rename may change compiler executable resolution at "
                     "{0}",
                     Rename.NewPath);
    for (PathRef CanonicalNew : CanonicalNewPaths)
      if (MayShadow(CanonicalNew))
        return error("file rename may change compiler executable resolution at "
                     "{0}",
                     CanonicalNew);
  } else if (!llvm::sys::path::is_absolute(Executable)) {
    llvm::SmallString<256> Absolute(Command.Directory);
    llvm::sys::path::append(Absolute, Executable);
    llvm::sys::path::remove_dots(Absolute, /*remove_dot_dot=*/true);
    ResolvedExecutable = Absolute.str().str();
    Executable = ResolvedExecutable;
  }
  if (auto Err =
          CheckPathFrom(Executable, Command.Directory, "compiler executable"))
    return Err;
  llvm::SmallString<256> RealExecutable;
  if (std::error_code EC = llvm::sys::fs::real_path(Executable, RealExecutable))
    return error("cannot identify compiler executable {0}: {1}", Executable,
                 EC.message());
  if (!isClangDriverName(RealExecutable))
    return error("file rename requires a Clang compiler, but {0} resolves to "
                 "{1}",
                 Command.CommandLine.front(), RealExecutable);
  for (llvm::StringRef Arg : llvm::ArrayRef(Command.CommandLine).drop_front())
    if (Arg.starts_with("@"))
      return error("cannot prove compiler response file after rename: {0}",
                   Arg);

  struct ForcedInput {
    std::string Name;
    std::string Option;
  };
  llvm::SmallVector<ForcedInput> OriginalForcedInputs;
  llvm::SmallVector<ForcedInput> DerivedForcedInputs;
  llvm::SmallVector<Path> OriginalSearchDirectories;
  llvm::SmallVector<Path> DerivedSearchDirectories;
  auto RecordSearchDirectory = [&](llvm::StringRef Value, bool Derived) {
    llvm::SmallString<256> Absolute(Value);
    if (!llvm::sys::path::is_absolute(Absolute)) {
      Absolute = Normalized->EffectiveDirectory;
      llvm::sys::path::append(Absolute, Value);
    }
    llvm::sys::path::remove_dots(Absolute, /*remove_dot_dot=*/true);
    (Derived ? DerivedSearchDirectories : OriginalSearchDirectories)
        .push_back(Absolute.str().str());
  };

  std::function<llvm::Error(llvm::ArrayRef<llvm::StringRef>,
                            llvm::opt::Visibility, unsigned, bool)>
      ParseAndCheck;
  ParseAndCheck = [&](llvm::ArrayRef<llvm::StringRef> Args,
                      llvm::opt::Visibility Visibility, unsigned Depth,
                      bool DerivedInvocation) -> llvm::Error {
    if (Depth > 8)
      return error("compiler option forwarding is nested too deeply");
    for (llvm::StringRef Arg : Args)
      if (Arg.contains("_Pragma") || Arg.contains("__pragma") ||
          Arg.contains("##") || Arg.contains("__has_") ||
          Arg.contains("__has_include") || Arg.contains("__has_include_next") ||
          Arg.contains("__has_embed"))
        return error("cannot prove preprocessor dependency state from compiler "
                     "argument {0}",
                     Arg);
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
      if (ID == options::OPT_analyzer_config)
        return error("cannot prove file dependencies of analyzer configuration "
                     "option {0}",
                     Arg->getSpelling());
      if (isPluginOption(ID))
        return error("cannot prove file dependencies of compiler plugin option "
                     "{0}",
                     Arg->getSpelling());
      if (isPrecompiledInputOption(ID))
        return error("cannot prove transitive dependencies of precompiled "
                     "compiler input {0}",
                     Arg->getSpelling());
      if (isOpaqueForwardingOption(ID) ||
          Arg->getOption().hasFlag(options::LinkerInput))
        return error("cannot prove file dependencies of opaque compiler option "
                     "{0}",
                     Arg->getSpelling());
      PathSemantics Semantics = pathSemantics(ID);
      if (Semantics == PathSemantics::None)
        continue;
      if (Semantics == PathSemantics::PathList) {
        for (llvm::StringRef Path : Arg->getValues())
          if (auto Err = CheckPath(Path, Arg->getSpelling()))
            return Err;
        continue;
      }
      if (Semantics == PathSemantics::ImplicitProfile) {
        if (auto Err = CheckPath("default.profdata", Arg->getSpelling()))
          return Err;
        continue;
      }
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
        if (isNormalHeaderSearchOption(ID))
          RecordSearchDirectory(Expanded, DerivedInvocation);
        continue;
      }
      switch (Semantics) {
      case PathSemantics::None:
        llvm_unreachable("handled above");
      case PathSemantics::Path:
        if (!Value.empty())
          if (auto Err = CheckPath(Value, Arg->getSpelling()))
            return Err;
        if (!Value.empty() && isNormalHeaderSearchOption(ID))
          RecordSearchDirectory(Value, DerivedInvocation);
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
            llvm::sys::path::has_parent_path(Value)) {
          if (auto Err = CheckPath(Value, Arg->getSpelling()))
            return Err;
        } else {
          (DerivedInvocation ? DerivedForcedInputs : OriginalForcedInputs)
              .push_back({Value.str(), Arg->getSpelling().str()});
        }
        break;
      case PathSemantics::HeaderSpelling:
        if (auto Err = CheckHeaderSpelling(Value, Arg->getSpelling()))
          return Err;
        break;
      case PathSemantics::PathList:
      case PathSemantics::ImplicitProfile:
        llvm_unreachable("handled above");
      }
    }
    if (!ForwardedDriver.empty())
      if (auto Err = ParseAndCheck(ForwardedDriver,
                                   llvm::opt::Visibility(options::ClangOption),
                                   Depth + 1, DerivedInvocation))
        return Err;
    if (!ForwardedCC1.empty())
      if (auto Err = ParseAndCheck(ForwardedCC1,
                                   llvm::opt::Visibility(options::CC1Option),
                                   Depth + 1, DerivedInvocation))
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
  if (auto Err = ParseAndCheck(OriginalArgs, Visibility, 0, false))
    return Err;

  if (!DriverDerivedCC1Args.empty()) {
    llvm::SmallVector<llvm::StringRef> Derived;
    Derived.reserve(DriverDerivedCC1Args.size());
    for (llvm::StringRef Arg : DriverDerivedCC1Args)
      Derived.push_back(Arg);
    if (!Derived.empty() && Derived.front() == "-cc1")
      Derived.erase(Derived.begin());
    if (auto Err = ParseAndCheck(
            Derived, llvm::opt::Visibility(options::CC1Option), 0, true))
      return Err;
  }

  if (FS) {
    llvm::ArrayRef<ForcedInput> ForcedInputs =
        DerivedForcedInputs.empty() ? llvm::ArrayRef(OriginalForcedInputs)
                                    : llvm::ArrayRef(DerivedForcedInputs);
    llvm::ArrayRef<Path> SearchDirectories =
        DerivedSearchDirectories.empty()
            ? llvm::ArrayRef(OriginalSearchDirectories)
            : llvm::ArrayRef(DerivedSearchDirectories);
    auto Identity = [&](PathRef Candidate, bool AfterRename)
        -> llvm::Expected<std::optional<llvm::sys::fs::UniqueID>> {
      if (AfterRename) {
        for (const auto &Mapping : ExpandedRenames)
          if (pathEqual(Candidate, Mapping.NewPath))
            return Mapping.OldIdentity;
        for (const auto &Mapping : ExpandedRenames)
          if (pathEqual(Candidate, Mapping.OldPath))
            return std::nullopt;
      }
      auto Status = FS->status(Candidate);
      if (Status)
        return Status->isRegularFile() ? std::optional<llvm::sys::fs::UniqueID>(
                                             Status->getUniqueID())
                                       : std::nullopt;
      if (Status.getError() == std::errc::no_such_file_or_directory)
        return std::nullopt;
      return error("cannot inspect forced compiler input candidate {0}: {1}",
                   Candidate, Status.getError().message());
    };
    auto Resolve = [&](llvm::StringRef Name, bool AfterRename)
        -> llvm::Expected<std::optional<llvm::sys::fs::UniqueID>> {
      llvm::SmallVector<Path> Directories;
      Directories.push_back(Normalized->EffectiveDirectory);
      llvm::append_range(Directories, SearchDirectories);
      for (PathRef Directory : Directories) {
        llvm::SmallString<256> Candidate(Directory);
        llvm::sys::path::append(Candidate, Name);
        llvm::sys::path::remove_dots(Candidate, /*remove_dot_dot=*/true);
        auto Found = Identity(Candidate, AfterRename);
        if (!Found)
          return Found.takeError();
        if (*Found)
          return *Found;
      }
      return std::nullopt;
    };
    for (const ForcedInput &Input : ForcedInputs) {
      auto Before = Resolve(Input.Name, false);
      if (!Before)
        return Before.takeError();
      auto After = Resolve(Input.Name, true);
      if (!After)
        return After.takeError();
      if (!*Before)
        return error("cannot resolve forced compiler input {0} from option {1}",
                     Input.Name, Input.Option);
      if (!*After || **Before != **After)
        return error("forced compiler input {0} from option {1} resolves to a "
                     "different file after rename",
                     Input.Name, Input.Option);
    }
  }
  return llvm::Error::success();
}

} // namespace clangd
} // namespace clang
