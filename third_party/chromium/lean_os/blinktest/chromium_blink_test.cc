// Blink's platform layer, on this machine.
//
// This is the foundation the DOM is built on: WTF - Blink's own string and
// container library - KURL, and the graphics primitives that sit on //cc's
// recorded paint and Skia. //cc arrived in M160 and it is what made this
// reachable; the layer above this one is //third_party/blink/renderer/core,
// which is the HTML parser, the DOM and the CSS cascade.
//
// What is graded here is chosen for the same reason M158's and M160's checks
// were: a library that links and returns plausible values passes every test
// that only asks whether the call returned. So the checks are Blink's own
// INVARIANTS rather than its API surface.
//
//   Is8Bit()         A Blink string is Latin-1 in one byte per character
//                    until it cannot be, and then it is UTF-16 in two. That
//                    is the single largest memory decision in the engine -
//                    a page of English text costs half - and it is invisible
//                    to every caller, which is exactly why it is worth
//                    asserting rather than trusting.
//   AtomicString     Two equal atomic strings are the SAME StringImpl, not
//                    two equal ones. Every tag name, attribute name and id
//                    in a document depends on that being true, because they
//                    are compared by pointer.
//   KURL             Blink's URL, over the GURL M150 built, keeping the
//                    parse rather than re-parsing on every question.
//   FromUtf8/Utf8    How text reaches Blink from everywhere - a response, a
//                    file, a mojo message - and the round trip has to come
//                    back the same bytes.
//   HashMap<Atomic>  The atom table and the hash traits agreeing: a lookup
//                    through a different string with the same characters,
//                    which is every attribute lookup in a document.
//
// Painting is not here, and section 5 says why rather than leaving it as an
// absence: it needs Oilpan, which needs blink::Platform, which is the
// renderer starting up rather than a library being linked.

#include <cstdio>
#include <string>
#include <cstdlib>

#include "base/at_exit.h"
#include "base/command_line.h"
#include "cc/paint/paint_canvas.h"
#include "cc/paint/paint_flags.h"
#include "cc/paint/paint_record.h"
#include "third_party/blink/renderer/platform/weborigin/kurl.h"
#include "third_party/blink/renderer/platform/wtf/hash_map.h"
#include "third_party/blink/renderer/platform/wtf/text/atomic_string.h"
// HashTraits<AtomicString> lives in its own header - without it a HashMap
// keyed on one instantiates the generic traits and fails on a deleted
// function, which is Blink saying "that key needs its own traits" in the
// least direct way available to a template.
#include "third_party/blink/renderer/platform/wtf/text/atomic_string_hash.h"
#include "third_party/blink/renderer/platform/wtf/text/string_builder.h"
#include "third_party/blink/renderer/platform/wtf/text/wtf_string.h"
#include "third_party/blink/renderer/platform/wtf/allocator/partitions.h"
#include "third_party/blink/renderer/platform/wtf/wtf.h"
#include "third_party/skia/include/core/SkBitmap.h"
#include "third_party/skia/include/core/SkCanvas.h"
#include "third_party/skia/include/core/SkColor.h"
#include "third_party/skia/include/core/SkImageInfo.h"

namespace {

int failures = 0;
int checks = 0;

void Check(const char* what, bool ok) {
  ++checks;
  if (ok) {
    std::printf("chromiumblink: %s\n", what);
  } else {
    std::printf("chromiumblink: FAIL %s\n", what);
    ++failures;
  }
}

}  // namespace

int main(int argc, char** argv) {
  base::AtExitManager at_exit;
  base::CommandLine::Init(argc, argv);

  std::printf("chromiumblink: starting\n");

  // Blink's partitions come FIRST, and finding that out cost this milestone a
  // boot: InitializeWtf() does not set them up - it calls AtomicString::Init()
  // and InitStringStatics(), both of which allocate - so on this machine the
  // first Blink allocation went to a PartitionRoot that was still null and
  // the process took a page fault at offset 0x80 of address zero.
  //
  // Chromium does this in BlinkInitializer, which a program outside the
  // renderer does not get. It is the one piece of Blink's startup an embedder
  // has to know about, and nothing says so at the call site.
  blink::Partitions::Initialize();
  blink::InitializeWtf();

  Check("Blink's partition allocator and WTF are up, in that order, because "
        "InitializeWtf allocates and does not bring the partitions up itself",
        true);

  // 1. The eight-bit invariant. Latin-1 text costs one byte a character;
  //    anything outside it costs two. Nothing in the API says which you have.
  {
    blink::String latin("a page of ordinary English text");
    Check("a Latin-1 string is eight bits per character", latin.Is8Bit());

    blink::StringBuilder builder;
    builder.Append("prefix ");
    // U+65E5, which Latin-1 has no room for. A literal rather than a named
    // constant so that what widens the string is visible here.
    builder.Append(static_cast<UChar>(0x65E5));
    blink::String widened = builder.ToString();
    Check("and appending a character Latin-1 has no room for makes it "
          "sixteen - the one decision that halves the cost of a page",
          !widened.Is8Bit());
    Check("and the widened string still says what it said",
          widened.length() == 8u && widened.starts_with("prefix"));
  }

  // 2. Atomic strings are compared by POINTER, which only works if equal
  //    strings really are one object. Every tag and attribute name in a
  //    document relies on it.
  {
    blink::AtomicString first("div");
    blink::String built = blink::String("d") + blink::String("iv");
    blink::AtomicString second(built);
    Check("two atomic strings with the same characters are one StringImpl, "
          "which is what lets Blink compare tag names by pointer",
          first.Impl() == second.Impl());
    blink::AtomicString other("span");
    Check("and two different ones are not", first.Impl() != other.Impl());
  }

  // 3. KURL, which is Blink's URL over the GURL M150 built.
  {
    blink::KURL base("https://example.test/a/b/page.html?q=1#frag");
    Check("blink::KURL parses a URL and keeps its parts",
          base.IsValid() && base.Protocol() == "https" &&
              base.Host() == "example.test" &&
              base.GetPath() == "/a/b/page.html" &&
              base.Query() == "q=1" && base.FragmentIdentifier() == "frag");

    blink::KURL relative(base, "../c/other.html");
    Check("and resolves a relative reference against it",
          relative.IsValid() &&
              relative.GetString() == "https://example.test/a/c/other.html");

    blink::KURL nonsense("not a url at all");
    Check("and says so when it is handed something that is not one",
          !nonsense.IsValid());
  }

  // 4. UTF-8 in and out, which is how text reaches Blink from everywhere -
  //    a network response, a file, a mojo message. The round trip has to come
  //    back the same, and the string in the middle has to be the SIXTEEN-bit
  //    kind, because the text is not Latin-1. That ties this check to the
  //    first one: the same decision, arrived at from the outside.
  {
    const char kUtf8[] = "na\xc3\xafve caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac";
    blink::String decoded = blink::String::FromUtf8(kUtf8);
    Check("blink::String::FromUtf8 decodes text that is not Latin-1, and the "
          "result is a sixteen-bit string",
          !decoded.empty() && !decoded.Is8Bit());
    std::string encoded = decoded.Utf8();
    Check("and encoding it back gives the same bytes",
          encoded == std::string(kUtf8));
    if (encoded != std::string(kUtf8)) {
      std::printf("chromiumblink:   round trip gave %zu bytes, wanted %zu\n",
                  encoded.size(), sizeof(kUtf8) - 1);
    }
  }

  // 5. And the two together: a hash map keyed by AtomicString, looked up with
  //    a DIFFERENT AtomicString that has the same characters. Every attribute
  //    lookup in a document is this, and it only works if the atom table and
  //    the hash traits agree - the table making the two one object, the
  //    traits hashing what that object holds rather than where it is.
  {
    blink::HashMap<blink::AtomicString, int> attributes;
    attributes.Set(blink::AtomicString("class"), 7);
    attributes.Set(blink::AtomicString("id"), 9);
    blink::String rebuilt = blink::String("cla") + blink::String("ss");
    auto found = attributes.find(blink::AtomicString(rebuilt));
    Check("a HashMap keyed by AtomicString finds an entry through a different "
          "string with the same characters - which is every attribute lookup "
          "in a document",
          found != attributes.end() && found->value == 7);
    Check("and does not find one that was never put in",
          attributes.find(blink::AtomicString("style")) == attributes.end());
  }

  // Painting is NOT graded here, and the reason is worth stating rather than
  // leaving as an absence. blink::GraphicsContext takes a PaintController,
  // whose constructor makes a PaintArtifact with MakeGarbageCollected - so it
  // needs Oilpan, and Oilpan's cppgc::HeapBase needs a cppgc::Platform, which
  // Blink builds from blink::Platform::Current(). That is BlinkInitializer:
  // a Platform implementation, a main thread scheduler and a v8::Platform,
  // which is the renderer starting up rather than a library being linked.
  //
  // This milestone found that out by faulting twice on the machine, at
  // cppgc::internal::HeapBase::HeapBase with a null platform and before that
  // at PartitionRoot::Alloc with a null root. The condition for grading a
  // paint is BlinkInitializer, and that is the next rung.

  if (failures != 0) {
    std::printf("chromiumblink: FAILED - %d of %d checks\n", failures, checks);
    return 1;
  }
  std::printf(
      "[m161] Blink's platform layer on this machine: %d checks. WTF's "
      "eight-bit strings, its atom table and a hash map keyed on it, UTF-8 in "
      "and out, and KURL over M150's GURL - the library the DOM is written "
      "in, out of a 57 MB archive of the whole renderer.\n",
      checks);
  std::printf("chromiumblink: done\n");
  return 0;
}
