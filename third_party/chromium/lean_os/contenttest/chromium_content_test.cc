// //content on this machine: the layer that takes a process over.
//
// Everything below this rung in the port is something an embedder calls -
// //base, //net, V8, Skia, //cc, Blink, viz. //content is the opposite shape.
// An embedder hands it main() and //content decides what this process is,
// brings up the machinery that kind of process needs, and calls the embedder
// back. There is no way to link it and then do one thing with it, which is
// why M163's -k 0 build and M164's first link found what they found: a
// library is graded by the calls you make into it, and a framework is graded
// by whether its own startup reaches the end.
//
// WHAT IS GRADED
//
// Not //content's API surface, which would pass on a build that linked and
// did nothing. The invariants of its startup, which a program that merely
// links has none of:
//
//   - the process type this process was dispatched as, which is the one
//     decision //content exists to make. A browser here and a renderer in a
//     second copy of this program, so a dispatch that always answered
//     "browser" is a failure rather than half a pass.
//   - the things ContentMainRunner::Initialize is responsible for creating:
//     the command line, the FeatureList, the thread pool, the ContentClient,
//     mojo, and the locked URL scheme registry. Every one of them is null or
//     absent in a process that has only linked //content.
//   - the ORDER the delegate's hooks arrive in, because that order is the
//     contract //content has with an embedder and nothing at the call site
//     says what it is.
//
// THE SECOND PROCESS
//
// A renderer is a different path through the same Initialize, and the
// interesting half: it is where a browser runs somebody else's code. This
// program spawns a second copy of itself with --type=renderer through
// base::LaunchProcess, which is M149's mechanism, and the child's exit code
// says which process type //content told it it was. The child declines to run
// the renderer's actual main - RunProcess returning an int is how an embedder
// says it handled the request - because a renderer with no mojo invitation
// has no browser to talk to, and this milestone is about the dispatch rather
// than about the renderer.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <variant>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/files/file_path.h"
#include "base/process/launch.h"
#include "base/process/process.h"
#include "base/task/thread_pool/thread_pool_instance.h"
#include "base/time/time.h"
#include "content/public/app/content_main.h"
#include "content/public/app/content_main_delegate.h"
#include "content/public/app/content_main_runner.h"
#include "content/public/common/content_client.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/main_function_params.h"
#include "url/url_util.h"

namespace {

int checks = 0;
int failures = 0;

void Check(bool ok, const char* what) {
  checks++;
  if (ok) {
    std::printf("chromiumcontent: %s\n", what);
  } else {
    failures++;
    std::printf("chromiumcontent: FAIL %s\n", what);
  }
}

// The hooks //content calls on its way up, in the order it calls them. The
// numbers are not an enum with gaps on purpose - what is being recorded is a
// sequence, and the check below is that the sequence is the one Chromium
// documents rather than that each hook was reached at all.
enum Hook {
  kBasicStartupComplete = 1,
  kPreSandboxStartup = 2,
  kPostEarlyInitialization = 3,
  kRunProcess = 4,
};

// Plain arrays rather than std::vector and std::string, because a file-scope
// object with a destructor is an exit-time destructor, which Chromium builds
// with -Wexit-time-destructors to forbid: the order they run in across
// translation units is not defined, and //content is still shutting down.
int hooks_seen[8];
int hooks_seen_count = 0;
char dispatched_process_type[64] = "<never dispatched>";
bool dispatched_as_browser = false;
bool dispatched_as_child = false;
bool run_process_saw_command_line = false;
content::ContentClient* client_given_to_content = nullptr;

void RecordHook(int hook) {
  if (hooks_seen_count <
      static_cast<int>(sizeof(hooks_seen) / sizeof(hooks_seen[0]))) {
    hooks_seen[hooks_seen_count] = hook;
  }
  hooks_seen_count++;
}

class LeanOsContentClient : public content::ContentClient {
 public:
  LeanOsContentClient() = default;
  ~LeanOsContentClient() override = default;
};

class LeanOsMainDelegate : public content::ContentMainDelegate {
 public:
  std::optional<int> BasicStartupComplete() override {
    RecordHook(kBasicStartupComplete);
    return std::nullopt;
  }

  void PreSandboxStartup() override { RecordHook(kPreSandboxStartup); }

  std::optional<int> PostEarlyInitialization(InvokedIn invoked_in) override {
    RecordHook(kPostEarlyInitialization);
    // The variant //content hands over IS the dispatch, made before any
    // process-specific main has run. A port that got this wrong would still
    // reach RunProcess and still be handed a process type string.
    dispatched_as_browser =
        std::holds_alternative<InvokedInBrowserProcess>(invoked_in);
    dispatched_as_child =
        std::holds_alternative<InvokedInChildProcess>(invoked_in);
    return std::nullopt;
  }

  std::variant<int, content::MainFunctionParams> RunProcess(
      const std::string& process_type,
      content::MainFunctionParams main_function_params) override {
    RecordHook(kRunProcess);
    std::snprintf(dispatched_process_type, sizeof(dispatched_process_type),
                  "%s", process_type.c_str());
    run_process_saw_command_line =
        main_function_params.command_line != nullptr &&
        main_function_params.command_line ==
            base::CommandLine::ForCurrentProcess();
    // An int is an embedder saying it handled the request, which stops
    // //content short of the browser's message loop and the renderer's.
    return 0;
  }

  content::ContentClient* CreateContentClient() override {
    client_given_to_content = &content_client_;
    return &content_client_;
  }

 private:
  LeanOsContentClient content_client_;
};

// What the child returns to say what //content made of it. Small and distinct
// so an exit code from somewhere else - a signal, a fatal initialisation
// error, a process that never reached main - cannot be read as a pass.
constexpr int kChildSawRenderer = 41;
constexpr int kChildSawSomethingElse = 42;

int RunContent(int argc, const char** argv) {
  content::ContentMainParams params(nullptr);
  LeanOsMainDelegate delegate;
  params.delegate = &delegate;
  params.argc = argc;
  params.argv = argv;

  std::unique_ptr<content::ContentMainRunner> runner =
      content::ContentMainRunner::Create();
  if (!runner) {
    return -1;
  }
  int initialize_result = runner->Initialize(std::move(params));
  if (initialize_result >= 0) {
    // A non-negative result is //content asking the process to exit, which is
    // an initialisation that did not finish.
    std::printf("chromiumcontent: Initialize asked for exit %d\n",
                initialize_result);
    return initialize_result;
  }
  int run_result = runner->Run();
  runner->Shutdown();
  return run_result;
}

}  // namespace

int main(int argc, const char** argv) {
  base::CommandLine::Init(argc, const_cast<char**>(argv));
  const bool is_child =
      base::CommandLine::ForCurrentProcess()->HasSwitch(switches::kProcessType);

  if (is_child) {
    if (RunContent(argc, argv) != 0) {
      return kChildSawSomethingElse;
    }
    return (std::strcmp(dispatched_process_type,
                        switches::kRendererProcess) == 0 &&
            dispatched_as_child && !dispatched_as_browser)
               ? kChildSawRenderer
               : kChildSawSomethingElse;
  }

  std::printf("chromiumcontent: starting\n");

  const int run_result = RunContent(argc, argv);
  Check(run_result == 0, "ContentMainRunner initialized, ran and shut down");

  // The dispatch. A browser process is the one with no --type switch at all,
  // which is why the string is empty rather than "browser".
  Check(dispatched_process_type[0] == '\0',
        "this process was dispatched as the browser (an empty process type)");
  Check(dispatched_as_browser && !dispatched_as_child,
        "and PostEarlyInitialization was told so before any process main ran");

  // The order, which nothing at the call site states.
  const int expected_hooks[] = {kBasicStartupComplete, kPreSandboxStartup,
                                kPostEarlyInitialization, kRunProcess};
  const int expected_count =
      static_cast<int>(sizeof(expected_hooks) / sizeof(expected_hooks[0]));
  bool order_ok = hooks_seen_count == expected_count;
  for (int i = 0; order_ok && i < expected_count; i++) {
    order_ok = hooks_seen[i] == expected_hooks[i];
  }
  if (!order_ok) {
    std::printf("chromiumcontent:   hooks were");
    for (int i = 0; i < hooks_seen_count && i < 8; i++) {
      std::printf(" %d", hooks_seen[i]);
    }
    std::printf(", expected");
    for (int i = 0; i < expected_count; i++) {
      std::printf(" %d", expected_hooks[i]);
    }
    std::printf("\n");
  }
  Check(order_ok,
        "the delegate's hooks arrived in the order //content documents");

  Check(run_process_saw_command_line,
        "RunProcess was handed this process's own command line");

  // What Initialize is responsible for creating. Every one of these is null in
  // a process that linked //content and called nothing.
  Check(base::FeatureList::GetInstance() != nullptr,
        "//content created the FeatureList");
  Check(base::ThreadPoolInstance::Get() != nullptr,
        "//content created the thread pool");
  // GetContentClient() itself is declared only under CONTENT_IMPLEMENTATION -
  // Chromium saying an embedder does not get to ask - so the exported
  // accessor is the one with ForTesting in its name, which this program is.
  // Comparing the POINTER rather than asking for non-null is the check:
  // //content installs a default client of its own when an embedder's
  // CreateContentClient returns null, and that would pass a null test.
  Check(client_given_to_content != nullptr &&
            content::GetContentClientForTesting() == client_given_to_content,
        "//content installed the ContentClient this program gave it");

  // url::LockSchemeRegistries() is part of //content's startup, and the
  // registry it locks is M150's //url. A program that never started //content
  // has an unlocked registry with the same schemes in it, so what is asked
  // here is the lock rather than the schemes.
  Check(url::IsStandard("https"),
        "the URL scheme registry //content locked still knows https");

  // The second process, and the one that matters: a renderer is a different
  // path through the same Initialize.
  base::FilePath self(base::CommandLine::ForCurrentProcess()->GetProgram());
  base::CommandLine child(self);
  child.AppendSwitchASCII(switches::kProcessType,
                          switches::kRendererProcess);
  base::LaunchOptions options;
  base::Process child_process = base::LaunchProcess(child, options);
  Check(child_process.IsValid(), "a second copy of this program was spawned");

  int child_exit = -1;
  bool child_finished =
      child_process.IsValid() &&
      child_process.WaitForExitWithTimeout(base::Seconds(60), &child_exit);
  Check(child_finished, "and it exited");
  if (child_finished && child_exit != kChildSawRenderer) {
    std::printf("chromiumcontent:   the child exited %d, wanted %d\n",
                child_exit, kChildSawRenderer);
  }
  Check(child_finished && child_exit == kChildSawRenderer,
        "//content dispatched THAT process as a renderer, not as a browser");

  if (failures) {
    std::printf("chromiumcontent: FAILED - %d of %d checks\n", failures,
                checks);
    return 1;
  }
  std::printf(
      "[m165] //content on this machine: %d checks. One process dispatched as "
      "the browser and a second as a renderer, the delegate's hooks in the "
      "order //content documents, and the FeatureList, thread pool, "
      "ContentClient and locked scheme registry its startup is responsible "
      "for.\n",
      checks);
  std::printf("chromiumcontent: done\n");
  return 0;
}
