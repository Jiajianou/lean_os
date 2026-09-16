#include "check.h"

#include <libintl.h>
#include <locale.h>
#include <string.h>

/* M157. gettext(3) with no message catalogs. There is no host oracle worth
   having here - macOS ships no libintl at all and Homebrew's is a different
   implementation from glibc's - so what is graded is the specified fallback
   behaviour, which is the only behaviour this machine can ever exhibit:
   every lookup returns the message it was given, and it returns THE SAME
   POINTER, because a caller that stores the result must not be handed
   something with a shorter lifetime than its argument. */

TEST(libintl, a_lookup_with_no_catalog_returns_its_own_argument) {
    static const char message[] = "No description";
    CHECK(gettext(message) == message);
    CHECK(dgettext("fontconfig", message) == message);
    CHECK(dcgettext("fontconfig", message, LC_MESSAGES) == message);
    CHECK(dcgettext(NULL, message, LC_ALL) == message);
    CHECK_EQ(strcmp(gettext(message), "No description"), 0);
}

TEST(libintl, the_plural_rule_with_no_catalog_is_english) {
    static const char one[] = "%d font";
    static const char many[] = "%d fonts";
    CHECK(ngettext(one, many, 1) == one);
    CHECK(ngettext(one, many, 0) == many);
    CHECK(ngettext(one, many, 2) == many);
    CHECK(ngettext(one, many, 1000000) == many);
    CHECK(dngettext("d", one, many, 1) == one);
    CHECK(dcngettext("d", one, many, 3, LC_MESSAGES) == many);
}

TEST(libintl, the_domain_before_a_program_names_one_is_messages) {
    CHECK_EQ(strcmp(textdomain(NULL), "messages"), 0);
    CHECK_EQ(strcmp(textdomain("fontconfig"), "fontconfig"), 0);
    CHECK_EQ(strcmp(textdomain(NULL), "fontconfig"), 0);
    /* The empty name is the documented way back to the default, and is the
       one long-name case that is not a refusal. */
    CHECK_EQ(strcmp(textdomain(""), "messages"), 0);
    CHECK_EQ(strcmp(textdomain(NULL), "messages"), 0);
}

TEST(libintl, a_name_longer_than_the_table_holds_is_refused_not_truncated) {
    char enormous[512];
    memset(enormous, 'd', sizeof(enormous) - 1);
    enormous[sizeof(enormous) - 1] = '\0';
    CHECK(textdomain(enormous) == NULL);
    /* And the refusal left the old answer alone rather than half-writing
       over it, which is the failure a truncating copy would produce. */
    CHECK_EQ(strcmp(textdomain(NULL), "messages"), 0);
    CHECK(bindtextdomain(enormous, "/usr/share/locale") == NULL);
    CHECK(bindtextdomain("fontconfig", enormous) == NULL);
}

TEST(libintl, a_binding_is_remembered_and_asking_with_null_reads_it_back) {
    CHECK(bindtextdomain("m157", NULL) == NULL);
    CHECK_EQ(strcmp(bindtextdomain("m157", "/usr/share/locale"),
                    "/usr/share/locale"), 0);
    CHECK_EQ(strcmp(bindtextdomain("m157", NULL), "/usr/share/locale"), 0);
    CHECK_EQ(strcmp(bindtextdomain("m157", "/pkg/share/locale"),
                    "/pkg/share/locale"), 0);
    CHECK_EQ(strcmp(bindtextdomain("m157", NULL), "/pkg/share/locale"), 0);

    CHECK(bind_textdomain_codeset("m157", NULL) == NULL);
    CHECK_EQ(strcmp(bind_textdomain_codeset("m157", "UTF-8"), "UTF-8"), 0);
    CHECK_EQ(strcmp(bind_textdomain_codeset("m157", NULL), "UTF-8"), 0);
    /* Binding a codeset did not disturb the directory bound to the same
       domain - they are two fields of one binding, not two bindings. */
    CHECK_EQ(strcmp(bindtextdomain("m157", NULL), "/pkg/share/locale"), 0);

    CHECK(bindtextdomain(NULL, "/usr/share/locale") == NULL);
    CHECK(bindtextdomain("", "/usr/share/locale") == NULL);
    CHECK(bind_textdomain_codeset(NULL, "UTF-8") == NULL);
}

TEST(libintl, a_binding_the_table_has_no_room_for_is_refused) {
    /* The table is a fixed set of slots and there is no way to empty it, so
       this does not assert HOW MANY fit - a number that would depend on what
       every test before it happened to bind, which is how this test failed
       the first time it ran. It fills until one is refused and then grades
       the property that matters: a refusal must leave every binding already
       made exactly as it was, rather than taking somebody else's slot. */
    const char *first = bindtextdomain("fill0", "/zero");
    REQUIRE(first != NULL);
    CHECK_EQ(strcmp(first, "/zero"), 0);

    int refused_at = -1;
    for (int i = 1; i < 64; i++) {
        char name[16] = "fill00";
        name[4] = (char)('0' + i / 10);
        name[5] = (char)('0' + i % 10);
        if (bindtextdomain(name, "/somewhere") == NULL) {
            refused_at = i;
            break;
        }
    }
    CHECK(refused_at > 0);
    CHECK(refused_at < 64);

    CHECK_EQ(strcmp(bindtextdomain("fill0", NULL), "/zero"), 0);
    /* A domain already in the table is still reachable when it is full. */
    CHECK_EQ(strcmp(bindtextdomain("fill0", "/still"), "/still"), 0);
    /* And a full table still answers a query about a domain it does not
       hold with "no binding" rather than with somebody else's. */
    CHECK(bindtextdomain("never-bound", NULL) == NULL);
}
