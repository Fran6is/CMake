/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file Copyright.txt or https://cmake.org/licensing for details.  */

#include "cmDependsCompiler.h"

#include <algorithm>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <utility>

#include <cm/optional>
#include <cm/string_view>
#include <cm/vector>
#include <cmext/string_view>

#include "cmsys/FStream.hxx"

#include "cmFileTime.h"
#include "cmGccDepfileReader.h"
#include "cmGlobalUnixMakefileGenerator3.h"
#include "cmLocalUnixMakefileGenerator3.h"
#include "cmStringAlgorithms.h"
#include "cmSystemTools.h"

bool cmDependsCompiler::CheckDependencies(
  const std::string& internalDepFile, const std::vector<std::string>& depFiles,
  cmDepends::DependencyMap& dependencies,
  const std::function<bool(const std::string&)>& isValidPath)
{
  bool status = true;
  bool forceReadDeps = true;

  cmFileTime internalDepFileTime;
  // read cached dependencies stored in internal file
  if (cmSystemTools::FileExists(internalDepFile)) {
    internalDepFileTime.Load(internalDepFile);
    forceReadDeps = false;

    // read current dependencies
    cmsys::ifstream fin(internalDepFile.c_str());
    if (fin) {
      std::string line;
      std::string depender;
      std::vector<std::string>* currentDependencies = nullptr;
      while (std::getline(fin, line)) {
        if (line.empty() || line.front() == '#') {
          continue;
        }
        // Drop carriage return character at the end
        if (line.back() == '\r') {
          line.pop_back();
          if (line.empty()) {
            continue;
          }
        }
        // Check if this a depender line
        if (line.front() != ' ') {
          depender = std::move(line);
          currentDependencies = &dependencies[depender];
          continue;
        }
        // This is a dependee line
        if (currentDependencies != nullptr) {
          currentDependencies->emplace_back(line.substr(1));
        }
      }
      fin.close();
    }
  }

  // Now, update dependencies map with all new compiler generated
  // dependencies files
  cmFileTime depFileTime;
  for (auto dep = depFiles.begin(); dep != depFiles.end(); dep++) {
    const auto& source = *dep++;
    const auto& target = *dep++;
    const auto& format = *dep++;
    const auto& depFile = *dep;

    if (!cmSystemTools::FileExists(depFile)) {
      continue;
    }

    if (!forceReadDeps) {
      depFileTime.Load(depFile);
    }
    if (forceReadDeps || depFileTime.Newer(internalDepFileTime)) {
      status = false;
      if (this->Verbose) {
        cmSystemTools::Stdout(cmStrCat("Dependencies file \"", depFile,
                                       "\" is newer than depends file \"",
                                       internalDepFile, "\".\n"));
      }

      std::vector<std::string> depends;
      if (format == "msvc"_s) {
        cmsys::ifstream fin(depFile.c_str());
        if (!fin) {
          continue;
        }

        std::string line;
        if (!isValidPath) {
          // insert source as first dependency
          depends.push_back(source);
        }
        while (cmSystemTools::GetLineFromStream(fin, line)) {
          depends.emplace_back(std::move(line));
        }
      } else {
        auto deps = cmReadGccDepfile(depFile.c_str());
        if (!deps) {
          continue;
        }

        // dependencies generated by the compiler contains only one target
        depends = std::move(deps->front().paths);
        if (depends.empty()) {
          // unexpectedly empty, ignore it and continue
          continue;
        }

        // depending of the effective format of the dependencies file generated
        // by the compiler, the target can be wrongly identified as a
        // dependency so remove it from the list
        if (depends.front() == target) {
          depends.erase(depends.begin());
        }

        // ensure source file is the first dependency
        if (depends.front() != source) {
          cm::erase(depends, source);
          if (!isValidPath) {
            depends.insert(depends.begin(), source);
          }
        } else if (isValidPath) {
          // remove first dependency because it must not be filtered out
          depends.erase(depends.begin());
        }
      }

      if (isValidPath) {
        cm::erase_if(depends, isValidPath);
        // insert source as first dependency
        depends.insert(depends.begin(), source);
      }

      dependencies[target] = std::move(depends);
    }
  }

  return status;
}

void cmDependsCompiler::WriteDependencies(
  const cmDepends::DependencyMap& dependencies, std::ostream& makeDepends,
  std::ostream& internalDepends)
{
  // dependencies file consumed by make tool
  const auto& lineContinue = static_cast<cmGlobalUnixMakefileGenerator3*>(
                               this->LocalGenerator->GetGlobalGenerator())
                               ->LineContinueDirective;
  const auto& binDir = this->LocalGenerator->GetBinaryDirectory();
  cmDepends::DependencyMap makeDependencies(dependencies);
  std::unordered_set<cm::string_view> phonyTargets;

  // external dependencies file
  for (auto& node : makeDependencies) {
    auto& deps = node.second;
    std::transform(
      deps.cbegin(), deps.cend(), deps.begin(),
      [this, &binDir](const std::string& dep) {
        return LocalGenerator->ConvertToMakefilePath(
          this->LocalGenerator->MaybeConvertToRelativePath(binDir, dep));
      });

    makeDepends << this->LocalGenerator->ConvertToMakefilePath(node.first)
                << ": " << deps.front();
    // first dependency is the source, remove it because should not be declared
    // as phony target
    deps.erase(deps.begin());
    for (const auto& dep : deps) {
      makeDepends << ' ' << lineContinue << "  " << dep;
      phonyTargets.emplace(dep.data(), dep.length());
    }
    makeDepends << std::endl << std::endl;
  }

  // add phony targets
  for (const auto& target : phonyTargets) {
    makeDepends << std::endl << target << ':' << std::endl;
  }

  // internal dependencies file
  for (const auto& node : dependencies) {
    internalDepends << node.first << std::endl;
    for (const auto& dep : node.second) {
      internalDepends << ' ' << dep << std::endl;
    }
    internalDepends << std::endl;
  }
}

void cmDependsCompiler::ClearDependencies(
  const std::vector<std::string>& depFiles)
{
  for (auto dep = depFiles.begin(); dep != depFiles.end(); dep++) {
    dep += 3;
    cmSystemTools::RemoveFile(*dep);
  }
}
