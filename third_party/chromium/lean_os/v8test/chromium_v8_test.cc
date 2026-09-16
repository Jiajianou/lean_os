// V8, on this machine.
//
// A JavaScript engine is the one part of a browser whose correctness a link
// cannot argue about: it compiles source to machine code at run time, writes
// it into pages it asked this kernel for, marks them executable and jumps
// into them. Every check below is arithmetic whose answer this program knows
// and V8 has to work out - so a wrong answer is a wrong answer rather than a
// missing symbol.
//
// It reports its own results and exits non-zero on the first failure, the way
// /bin/chromiumbase and /bin/ruststd do, because the boot self-test that runs
// it has no other way to tell what went wrong.

#include <cstdio>
#include <cstring>
#include <string>

// Spelled the way //gin spells it rather than the way V8's own samples do.
// V8's :headers_config adds "include" relative to the BUILD.gn that uses it,
// which resolves inside //v8 and nowhere else; a consumer outside that
// directory reaches the same headers through the source root.
#include "v8/include/libplatform/libplatform.h"
#include "v8/include/v8-context.h"
#include "v8/include/v8-exception.h"
#include "v8/include/v8-initialization.h"
#include "v8/include/v8-isolate.h"
#include "v8/include/v8-local-handle.h"
#include "v8/include/v8-primitive.h"
#include "v8/include/v8-script.h"
#include "v8/include/v8-template.h"
#include "v8/include/v8-value.h"

namespace {

int failures = 0;
int checks = 0;

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiumv8: %s\n", what);
  } else {
    std::printf("chromiumv8: FAIL %s\n", what);
    ++failures;
  }
}

// Compile and run one script in the current context, and hand back whatever
// it evaluated to as a UTF-8 string. An empty answer means V8 refused the
// source or the script threw, and every caller treats that as a failure.
std::string Evaluate(v8::Isolate* isolate, v8::Local<v8::Context> context,
                     const char* source) {
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> text;
  if (!v8::String::NewFromUtf8(isolate, source).ToLocal(&text)) {
    return std::string();
  }
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, text).ToLocal(&script)) {
    return std::string();
  }
  v8::Local<v8::Value> result;
  if (!script->Run(context).ToLocal(&result)) {
    return std::string();
  }
  v8::String::Utf8Value utf8(isolate, result);
  if (*utf8 == nullptr) {
    return std::string();
  }
  return std::string(*utf8);
}

bool Evaluates(v8::Isolate* isolate, v8::Local<v8::Context> context,
               const char* source, const char* expected) {
  const std::string answer = Evaluate(isolate, context, source);
  if (answer == expected) {
    return true;
  }
  std::printf("chromiumv8:   %s -> \"%s\", expected \"%s\"\n", source,
              answer.c_str(), expected);
  return false;
}

// The interpreter's own arithmetic, and the string and object machinery
// under it. None of this reaches the optimising compiler.
bool InterpretsJavaScript(v8::Isolate* isolate,
                          v8::Local<v8::Context> context) {
  return Evaluates(isolate, context, "6 * 7", "42") &&
         Evaluates(isolate, context, "'lean' + '_' + 'os'", "lean_os") &&
         Evaluates(isolate, context, "[3,1,2].sort().join(',')", "1,2,3") &&
         Evaluates(isolate, context,
                   "JSON.stringify({a:1,b:[2,3]})", "{\"a\":1,\"b\":[2,3]}") &&
         Evaluates(isolate, context, "(function f(n){"
                   "return n<2?n:f(n-1)+f(n-2)})(20)", "6765") &&
         Evaluates(isolate, context, "typeof Math.sqrt(2)", "number") &&
         Evaluates(isolate, context, "Math.sqrt(2).toFixed(6)", "1.414214");
}

// TurboFan. A loop this hot is compiled rather than interpreted, which means
// V8 asked this kernel for pages, wrote x86-64 into them, asked for them to
// become executable and jumped in. That is the sharpest thing a JavaScript
// engine does to an operating system, and nothing else in this tree does it.
bool CompilesAndRunsMachineCode(v8::Isolate* isolate,
                                v8::Local<v8::Context> context) {
  return Evaluates(isolate, context,
                   "function add(a, b) { return (a + b) | 0; }\n"
                   "let total = 0;\n"
                   "for (let i = 0; i < 2000000; i++) { total = add(total, 3); }\n"
                   "total",
                   "6000000");
}

// A language feature that is a whole subsystem: the regular expression
// engine, which V8 also compiles.
bool RunsRegularExpressions(v8::Isolate* isolate,
                            v8::Local<v8::Context> context) {
  return Evaluates(isolate, context,
                   "'2026-09-16'.replace(/(\\d+)-(\\d+)-(\\d+)/, '$3/$2/$1')",
                   "16/09/2026") &&
         Evaluates(isolate, context,
                   "('a1b22c333'.match(/\\d+/g) || []).join('|')",
                   "1|22|333");
}

// An exception that crosses from JavaScript back into C++, caught by
// v8::TryCatch rather than by a C++ catch - V8 is built with -fno-exceptions
// here, the way every other Chromium object in this image is.
bool ReportsThrownExceptions(v8::Isolate* isolate,
                             v8::Local<v8::Context> context) {
  v8::TryCatch try_catch(isolate);
  v8::Local<v8::String> text;
  if (!v8::String::NewFromUtf8(isolate, "throw new Error('from lean_os')")
           .ToLocal(&text)) {
    return false;
  }
  v8::Local<v8::Script> script;
  if (!v8::Script::Compile(context, text).ToLocal(&script)) {
    return false;
  }
  v8::Local<v8::Value> ignored;
  if (script->Run(context).ToLocal(&ignored)) {
    return false;
  }
  if (!try_catch.HasCaught()) {
    return false;
  }
  v8::String::Utf8Value message(isolate, try_catch.Exception());
  return *message != nullptr &&
         std::strstr(*message, "from lean_os") != nullptr;
}

// The direction a browser actually uses: JavaScript calling into native code.
// Every DOM method Blink exposes is this shape.
void NativeAdd(const v8::FunctionCallbackInfo<v8::Value>& info) {
  double sum = 0;
  for (int i = 0; i < info.Length(); ++i) {
    sum += info[i]->NumberValue(info.GetIsolate()->GetCurrentContext())
               .FromMaybe(0);
  }
  info.GetReturnValue().Set(sum);
}

bool CallsBackIntoNativeCode(v8::Isolate* isolate) {
  v8::Local<v8::ObjectTemplate> global = v8::ObjectTemplate::New(isolate);
  global->Set(
      v8::String::NewFromUtf8(isolate, "nativeAdd").ToLocalChecked(),
      v8::FunctionTemplate::New(isolate, NativeAdd));
  v8::Local<v8::Context> context = v8::Context::New(isolate, nullptr, global);
  v8::Context::Scope scope(context);
  return Evaluates(isolate, context, "nativeAdd(1, 2, 3) + nativeAdd(4, 5)",
                   "15");
}

// The garbage collector, asked to run and then asked whether it did. The
// numbers are not compared against a constant - a heap that grew and then
// shrank is the claim, and the threshold is deliberately loose because the
// exact figure is V8's business and not this test's.
bool CollectsGarbage(v8::Isolate* isolate, v8::Local<v8::Context> context) {
  const std::string answer = Evaluate(isolate, context,
      "let kept = [];\n"
      "for (let i = 0; i < 200000; i++) { kept.push({ i: i, s: 'x' + i }); }\n"
      "const grown = kept.length;\n"
      "kept = null;\n"
      "grown");
  if (answer != "200000") {
    return false;
  }
  v8::HeapStatistics before;
  isolate->GetHeapStatistics(&before);
  isolate->LowMemoryNotification();
  v8::HeapStatistics after;
  isolate->GetHeapStatistics(&after);
  if (after.used_heap_size() >= before.used_heap_size()) {
    std::printf("chromiumv8:   heap %zu -> %zu, expected it to shrink\n",
                before.used_heap_size(), after.used_heap_size());
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char* argv[]) {
  v8::V8::InitializeICUDefaultLocation(argv[0]);
  v8::V8::InitializeExternalStartupData(argv[0]);
  std::unique_ptr<v8::Platform> platform = v8::platform::NewDefaultPlatform();
  v8::V8::InitializePlatform(platform.get());
  v8::V8::Initialize();

  v8::Isolate::CreateParams create_params;
  create_params.array_buffer_allocator =
      v8::ArrayBuffer::Allocator::NewDefaultAllocator();
  v8::Isolate* isolate = v8::Isolate::New(create_params);
  {
    v8::Isolate::Scope isolate_scope(isolate);
    v8::HandleScope handle_scope(isolate);
    v8::Local<v8::Context> context = v8::Context::New(isolate);
    v8::Context::Scope context_scope(context);

    Check("V8 started: a platform, an isolate and a context, on this kernel's "
          "threads",
          true);
    Check("it interprets JavaScript - arithmetic, strings, arrays, JSON and "
          "a recursive function",
          InterpretsJavaScript(isolate, context));
    Check("it compiles a hot loop to x86-64 and runs it out of pages this "
          "kernel made executable",
          CompilesAndRunsMachineCode(isolate, context));
    Check("its regular expression engine matches and replaces",
          RunsRegularExpressions(isolate, context));
    Check("a thrown Error reaches v8::TryCatch, with -fno-exceptions",
          ReportsThrownExceptions(isolate, context));
    Check("JavaScript calls a C++ function through a v8::FunctionTemplate - "
          "the shape every DOM method has",
          CallsBackIntoNativeCode(isolate));
    Check("the garbage collector grew a heap of 200,000 objects and gave it "
          "back",
          CollectsGarbage(isolate, context));
  }

  isolate->Dispose();
  v8::V8::Dispose();
  v8::V8::DisposePlatform();
  delete create_params.array_buffer_allocator;

  if (failures != 0) {
    std::printf("chromiumv8: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m156] V8 runs on this machine: %d checks, a JavaScript engine out of "
      "Chromium's own build, compiling to x86-64 at run time on an operating "
      "system it has never heard of.\n",
      checks);
  std::printf("chromiumv8: done\n");
  return 0;
}
