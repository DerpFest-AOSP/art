/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_PROFMAN_PROFILE_ASSISTANT_H_
#define ART_PROFMAN_PROFILE_ASSISTANT_H_

#include <string>
#include <vector>

#include "base/scoped_flock.h"
#include "profile/profile_compilation_info.h"
#include "profman/profman_result.h"

namespace art {

class ProfileAssistant {
 public:
  class Options {
   public:
    static constexpr bool kForceMergeDefault = false;
    static constexpr bool kBootImageMergeDefault = false;
    static constexpr uint32_t kMinNewMethodsPercentChangeForCompilation = 2;
    static constexpr uint32_t kMinNewClassesPercentChangeForCompilation = 2;

    Options()
        : force_merge_(kForceMergeDefault),
          force_merge_and_analyze_(kForceMergeDefault),
          boot_image_merge_(kBootImageMergeDefault),
          min_new_methods_percent_change_for_compilation_(
              kMinNewMethodsPercentChangeForCompilation),
          min_new_classes_percent_change_for_compilation_(
              kMinNewClassesPercentChangeForCompilation) {
    }

    // Only for S and T uses. U+ should use `IsForceMergeAndAnalyze`.
    bool IsForceMerge() const { return force_merge_; }
    bool IsForceMergeAndAnalyze() const { return force_merge_and_analyze_; }
    bool IsBootImageMerge() const { return boot_image_merge_; }
    uint32_t GetMinNewMethodsPercentChangeForCompilation() const {
        return min_new_methods_percent_change_for_compilation_;
    }
    uint32_t GetMinNewClassesPercentChangeForCompilation() const {
        return min_new_classes_percent_change_for_compilation_;
    }

    void SetForceMerge(bool value) { force_merge_ = value; }
    void SetForceMergeAndAnalyze(bool value) { force_merge_and_analyze_ = value; }
    void SetBootImageMerge(bool value) { boot_image_merge_ = value; }
    void SetMinNewMethodsPercentChangeForCompilation(uint32_t value) {
      min_new_methods_percent_change_for_compilation_ = value;
    }
    void SetMinNewClassesPercentChangeForCompilation(uint32_t value) {
      min_new_classes_percent_change_for_compilation_ = value;
    }

   private:
    // If true, performs a forced merge, without analyzing if there is a significant difference
    // between before and after the merge.
    // See ProfileAssistant#ProcessProfile.
    // Only for S and T uses. U+ should use `force_merge_and_analyze_`.
    bool force_merge_;
    // If true, performs a forced merge and analyzes if there is any difference between before and
    // after the merge.
    // See ProfileAssistant#ProcessProfile.
    bool force_merge_and_analyze_;
    // Signals that the merge is for boot image profiles. It will ignore differences
    // in profile versions (instead of aborting).
    bool boot_image_merge_;
    uint32_t min_new_methods_percent_change_for_compilation_;
    uint32_t min_new_classes_percent_change_for_compilation_;
  };

  // Process the profile information present in the given files. Returns one of
  // ProcessingResult values depending on profile information and whether or not
  // the analysis ended up successfully (i.e. no errors during reading,
  // merging or writing of profile files).
  //
  // When the returned value is kCompile there is a significant difference
  // between profile_files and reference_profile_files. In this case
  // reference_profile will be updated with the profiling info obtain after
  // merging all profiles.
  //
  // When the returned value is kSkipCompilationSmallDelta, the difference between
  // the merge of the current profiles and the reference one is insignificant. In
  // this case no file will be updated. A variation of this code is
  // kSkipCompilationEmptyProfiles which indicates that all the profiles are empty.
  // This allow the caller to make fine grain decisions on the compilation strategy.
  static ProfmanResult::ProcessingResult ProcessProfiles(
      const std::vector<std::string>& profile_files,
      const std::string& reference_profile_file,
      const ProfileCompilationInfo::ProfileLoadFilterFn& filter_fn
          = ProfileCompilationInfo::ProfileFilterFnAcceptAll,
      const Options& options = Options());

  static ProfmanResult::ProcessingResult ProcessProfiles(
      const std::vector<int>& profile_files_fd_,
      int reference_profile_file_fd,
      const ProfileCompilationInfo::ProfileLoadFilterFn& filter_fn
          = ProfileCompilationInfo::ProfileFilterFnAcceptAll,
      const Options& options = Options());

 private:
  static ProfmanResult::ProcessingResult ProcessProfilesInternal(
      const std::vector<ScopedFlock>& profile_files,
      const ScopedFlock& reference_profile_file,
      const ProfileCompilationInfo::ProfileLoadFilterFn& filter_fn,
      const Options& options);

  DISALLOW_COPY_AND_ASSIGN(ProfileAssistant);
};

}  // namespace art

#endif  // ART_PROFMAN_PROFILE_ASSISTANT_H_
