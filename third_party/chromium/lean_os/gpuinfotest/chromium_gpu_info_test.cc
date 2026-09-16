// What Chromium makes of a machine with no GPU, asked of the machine.
//
// M158 turned the GPU stack off and wrote down why. Every sentence in that
// argument was this project's, and the flags that carried it were this
// project's too. This program asks CHROMIUM the same question and grades its
// answer, which is a different kind of claim: //gpu/config is the code that
// decides, on every platform Chrome ships on, which GPU features a machine
// may use. It reads a blocklist compiled from software_rendering_list.json,
// a driver bug list compiled from gpu_driver_bug_list.json, and a GPUInfo
// collected from the system - and here the system is one with nothing to
// collect.
//
// Where the GPUInfo comes from is the question M158 left open and this
// milestone closes. The answer is ANGLE's own gpu_info_util: on a Linux-family
// build with no libpci, no X11 and no Vulkan to ask, angle::GetSystemInfo
// finds no devices and returns false - which is the truth about this machine,
// reached through a code path ANGLE already ships rather than through a file
// written here. gpu_info_collector_linux.cc is compiled unchanged.
//
// //gpu/config is also the gate to the whole rest of Chromium, which is worth
// saying because it does not sound like it. //cc reaches it through
// //components/viz/common; so does //media; so does //services/network's
// mojom; and so does //third_party/blink/renderer/platform/wtf - Blink's
// string library - through //third_party/blink/public/common:headers. There
// is nothing above //net in this port that is not behind this target.
//
// Every check here is a decision Chromium makes rather than a value this
// program supplies. That matters for the same reason M157 and M158 said it
// about pixels: a configuration library that links and returns zeroes passes
// every test that only asks whether the call returned.
//
// It earned that on its first run, by disagreeing. This program was written
// expecting ComputeGpuFeatureInfo - the blocklist applied to the GPUInfo
// collected above - to turn the GPU features off, and it does the opposite:
// it ENABLES accelerated 2D canvas, WebGL, WebGL2, accelerated GL and video
// decode and encode. That is the rule list being right. software_rendering_
// list.json is a list of hardware known to be BROKEN, and hardware that is
// not there matches none of it; a list with no rule for a machine says the
// same thing about it that it says about every machine it does not
// recognise. "This GPU is blocklisted" and "there is no GPU" are different
// questions, and gpu/config answers the second one with a different
// function. Checks 4 and 5 grade the difference in both directions, because
// a port that ran only the rule list would conclude this machine can do
// WebGL - and would have every other claim here still passing.

#include <cstdio>
#include <memory>
#include <cstdlib>
#include <string>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "gpu/config/gpu_blocklist.h"
#include "gpu/config/gpu_driver_bug_list.h"
#include "gpu/config/gpu_feature_info.h"
#include "gpu/config/gpu_feature_type.h"
#include "gpu/config/gpu_info.h"
#include "gpu/config/gpu_info_collector.h"
#include "gpu/config/gpu_preferences.h"
#include "gpu/config/gpu_util.h"
#include "ui/gl/gl_implementation.h"

namespace {

int failures = 0;
int checks = 0;

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiumgpu: %s\n", what);
  } else {
    std::printf("chromiumgpu: FAIL %s\n", what);
    ++failures;
  }
}

const char* StatusName(gpu::GpuFeatureStatus status) {
  switch (status) {
    case gpu::kGpuFeatureStatusEnabled:
      return "enabled";
    case gpu::kGpuFeatureStatusBlocklisted:
      return "blocklisted";
    case gpu::kGpuFeatureStatusDisabled:
      return "disabled";
    case gpu::kGpuFeatureStatusSoftware:
      return "software";
    case gpu::kGpuFeatureStatusUndefined:
      return "undefined";
    default:
      return "?";
  }
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  std::printf("chromiumgpu: starting\n");

  // 1. The collector, on this machine. CollectBasicGraphicsInfo is what the
  //    browser process calls at startup, before any GL context exists, and on
  //    a Linux-family build it is angle::GetSystemInfo underneath. With no
  //    libpci compiled in, no X11 and no Vulkan, that function's own first
  //    branch is "no PCI devices found" and it returns false. A machine with
  //    a GPU would take the other branch, so this is not a constant.
  gpu::GPUInfo collected;
  bool collected_ok = gpu::CollectBasicGraphicsInfo(&collected);
  Check("gpu::CollectBasicGraphicsInfo reports failure on a machine with no "
        "GPU to report",
        !collected_ok);

  Check("and it found no devices at all - no primary vendor, no secondaries",
        collected.gpu.vendor_id == 0u && collected.gpu.device_id == 0u &&
            collected.secondary_gpus.empty());
  if (!collected.secondary_gpus.empty() || collected.gpu.vendor_id != 0u) {
    std::printf("chromiumgpu:   vendor 0x%04x device 0x%04x, %zu secondary\n",
                collected.gpu.vendor_id, collected.gpu.device_id,
                collected.secondary_gpus.size());
  }

  // 2. The GL implementation. ui/gl has a word for "there is none" and this
  //    is a build in which nothing ever calls gl::init::InitializeGLOneOff,
  //    because there is no display for it to initialise against.
  Check("and gl::GetGLImplementation() is kGLImplementationNone",
        gl::GetGLImplementation() == gl::kGLImplementationNone);

  // 3. The blocklist itself. This is the generated
  //    software_rendering_list_autogen.cc - Chromium's real rule list, the
  //    one that decides on every machine Chrome runs on - compiled into this
  //    binary. A non-empty rule set is the difference between that data being
  //    there and the target having linked an empty array.
  std::unique_ptr<gpu::GpuBlocklist> blocklist = gpu::GpuBlocklist::Create();
  bool have_blocklist = blocklist != nullptr && blocklist->num_entries() > 0u;
  Check("gpu::GpuBlocklist::Create() built the software rendering list Chrome "
        "ships, and it has rules in it",
        have_blocklist);
  if (blocklist) {
    std::printf("chromiumgpu:   software rendering list: %zu entries, "
                "highest id %u\n",
                blocklist->num_entries(), blocklist->max_entry_id());
  }

  std::unique_ptr<gpu::GpuDriverBugList> bug_list =
      gpu::GpuDriverBugList::Create();
  Check("and gpu::GpuDriverBugList::Create() built the driver bug list too",
        bug_list != nullptr && bug_list->num_entries() > 0u);

  // 4. And now the thing this program was written expecting the opposite of.
  //
  //    ComputeGpuFeatureInfo runs the blocklist against a GPUInfo. Given the
  //    empty one collected above it ENABLES six features - accelerated 2D
  //    canvas, WebGL, WebGL2, accelerated GL, and video decode and encode -
  //    and that is not a bug in it or in this port. The software rendering
  //    list is a list of hardware known to be BROKEN, and hardware that is
  //    not there matches none of it. A rule list has nothing to say about a
  //    machine it has no rules for, so it says what it says about every
  //    machine it does not recognise: carry on.
  //
  //    "This GPU is blocklisted" and "there is no GPU" are different
  //    questions and gpu/config answers them with different functions. A
  //    port that ran only the rule list would conclude this machine can do
  //    WebGL. That is worth grading in both directions, so this checks that
  //    the rule list really does enable them - a check that FAILS if some
  //    future rule blocklists vendor 0, which would make every other claim
  //    here true for the wrong reason.
  gpu::GpuPreferences preferences;
  bool needs_more_info = true;
  gpu::GpuFeatureInfo from_rules = gpu::ComputeGpuFeatureInfo(
      collected, preferences, base::CommandLine::ForCurrentProcess(),
      &needs_more_info);

  Check("the rule list alone does NOT disable WebGL here - a blocklist of "
        "broken hardware has no rule for hardware that is absent",
        from_rules.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_WEBGL] ==
            gpu::kGpuFeatureStatusEnabled);

  // 5. The function that does answer it. This is what the browser process
  //    calls once it knows there is no GPU process worth starting, and it is
  //    Chromium's own name for this machine's situation.
  gpu::GpuFeatureInfo no_gpu = gpu::ComputeGpuFeatureInfoWithNoGpu();

  Check("gpu::ComputeGpuFeatureInfoWithNoGpu disables accelerated WebGL - "
        "which is the unsupported M158 chose over a context that paints "
        "nothing, said by Chromium rather than by args.gn",
        no_gpu.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_WEBGL] ==
            gpu::kGpuFeatureStatusDisabled &&
            no_gpu.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_WEBGL2] ==
                gpu::kGpuFeatureStatusDisabled &&
            no_gpu.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_GL] ==
                gpu::kGpuFeatureStatusDisabled);

  // 6. And the one it does not disable, which is the whole M158 decision in
  //    one value: the 2D canvas is kGpuFeatureStatusSOFTWARE rather than
  //    disabled. Chromium does not stop drawing because there is no GPU - it
  //    draws on the CPU, which is the path //cc and //viz keep for exactly
  //    this case.
  Check("and makes the 2D canvas kGpuFeatureStatusSoftware rather than "
        "disabled - not 'no drawing', but 'drawing on the CPU'",
        no_gpu.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_2D_CANVAS] ==
            gpu::kGpuFeatureStatusSoftware);

  bool any_no_gpu_enabled = false;
  for (int i = 0; i < gpu::NUMBER_OF_GPU_FEATURE_TYPES; ++i) {
    if (no_gpu.status_values[i] == gpu::kGpuFeatureStatusEnabled) {
      any_no_gpu_enabled = true;
      std::printf("chromiumgpu:   feature %d is still enabled\n", i);
    }
  }
  Check("and enables none of the 13 GPU features at all",
        !any_no_gpu_enabled);

  std::printf("chromiumgpu:   rule list -> webgl %s, gl %s; no-gpu -> "
              "webgl %s, gl %s, 2d canvas %s\n",
              StatusName(
                  from_rules.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_WEBGL]),
              StatusName(
                  from_rules.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_GL]),
              StatusName(
                  no_gpu.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_WEBGL]),
              StatusName(
                  no_gpu.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_GL]),
              StatusName(
                  no_gpu.status_values[gpu::GPU_FEATURE_TYPE_ACCELERATED_2D_CANVAS]));

  // 7. What Skia is told, asked of the answer that applies. M157 turned off
  //    skia_use_dawn and M158 turned off four more; this is gpu/config - the
  //    code a browser actually consults before it picks a graphics engine -
  //    reaching the same conclusion from this machine's own feature info.
  //    kNone is always supported; the other three are back ends there is no
  //    hardware for.
  Check("gpu/config supports GrContextType::kNone for this machine",
        gpu::IsGrContextTypeSupported(gpu::GrContextType::kNone, no_gpu));
  Check("and refuses kVulkan, kGraphiteDawn and kGL - so Skia rasterises on "
        "the CPU here because Chromium says so, not because args.gn does",
        !gpu::IsGrContextTypeSupported(gpu::GrContextType::kVulkan, no_gpu) &&
            !gpu::IsGrContextTypeSupported(gpu::GrContextType::kGraphiteDawn,
                                           no_gpu) &&
            !gpu::IsGrContextTypeSupported(gpu::GrContextType::kGL, no_gpu));

  if (failures != 0) {
    std::printf("chromiumgpu: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m159] Chromium's own GPU configuration, on this machine: %d checks. "
      "ANGLE's system information reader finds no devices, the blocklist has "
      "no rule for hardware that is absent, and the function that does answer "
      "that question turns all 13 features off and leaves the 2D canvas on "
      "the CPU - which is what //cc, //viz, //media and Blink ask before they "
      "start.\n",
      checks);
  std::printf("chromiumgpu: done\n");
  return 0;
}
