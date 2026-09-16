#include <libintl.h>

#include <locale.h>
#include <stddef.h>
#include <string.h>

/* There are no message catalogs on this machine and setlocale refuses every
   locale but C, so every lookup here returns the message it was given. That
   is not a stub: it is what gettext(3) is specified to do when no catalog
   for the domain can be found, which on a one-locale machine is always. A
   caller cannot tell this apart from a system with gettext installed and no
   translation for its language, because there is nothing to tell apart.

   The bindings are remembered anyway. bindtextdomain(domain, NULL) is how a
   program asks where a domain is bound, and answering with the directory it
   was told is the only truthful answer available - the alternative is to
   report a directory nothing was ever bound to. */

#define DOMAIN_NAME_MAX 64
#define DIRECTORY_MAX   256
#define CODESET_MAX     32
#define BINDINGS_MAX    8

typedef struct {
    char name[DOMAIN_NAME_MAX];
    char directory[DIRECTORY_MAX];
    char codeset[CODESET_MAX];
    int  has_directory;
    int  has_codeset;
} binding_t;

static binding_t bindings[BINDINGS_MAX];
static int binding_count;

/* "messages" is the domain a program is in before it names one, which is
   what textdomain(NULL) has to answer. */
static char current_domain[DOMAIN_NAME_MAX] = "messages";

static int copy_bounded(char *destination, size_t size, const char *source) {
    size_t length = strlen(source);
    if (length + 1 > size) {
        return 0;
    }
    memcpy(destination, source, length + 1);
    return 1;
}

/* `value` is what is about to be stored, and it is checked HERE rather than
   by the caller after the fact: a refusal that has already taken a slot is a
   refusal that costs somebody else their binding, and there are only eight.
   The host test found this by filling the table after a test that had made
   one refused call. */
static binding_t *binding_for(const char *domain_name, const char *value,
                              size_t value_size) {
    for (int i = 0; i < binding_count; i++) {
        if (strcmp(bindings[i].name, domain_name) == 0) {
            return &bindings[i];
        }
    }
    if (!value) {
        return NULL;
    }
    if (strlen(domain_name) + 1 > DOMAIN_NAME_MAX ||
        strlen(value) + 1 > value_size || binding_count == BINDINGS_MAX) {
        return NULL;
    }
    binding_t *fresh = &bindings[binding_count];
    copy_bounded(fresh->name, sizeof(fresh->name), domain_name);
    fresh->has_directory = 0;
    fresh->has_codeset = 0;
    binding_count++;
    return fresh;
}

char *gettext(const char *message_id) {
    return dcgettext(NULL, message_id, LC_MESSAGES);
}

char *dgettext(const char *domain_name, const char *message_id) {
    return dcgettext(domain_name, message_id, LC_MESSAGES);
}

char *dcgettext(const char *domain_name, const char *message_id, int category) {
    (void)domain_name;
    (void)category;
    return (char *)message_id;
}

char *ngettext(const char *message_id, const char *message_id_plural,
               unsigned long n) {
    return dcngettext(NULL, message_id, message_id_plural, n, LC_MESSAGES);
}

char *dngettext(const char *domain_name, const char *message_id,
                const char *message_id_plural, unsigned long n) {
    return dcngettext(domain_name, message_id, message_id_plural, n,
                      LC_MESSAGES);
}

char *dcngettext(const char *domain_name, const char *message_id,
                 const char *message_id_plural, unsigned long n, int category) {
    (void)domain_name;
    (void)category;
    /* The plural rule with no catalog to override it is English's, which is
       what gettext(3) falls back to: one is singular and everything else,
       zero included, is not. */
    return (char *)(n == 1 ? message_id : message_id_plural);
}

char *textdomain(const char *domain_name) {
    if (!domain_name) {
        return current_domain;
    }
    /* An empty name means "back to the default", which is the one case where
       a name this long is not a refusal. */
    if (domain_name[0] == '\0') {
        copy_bounded(current_domain, sizeof(current_domain), "messages");
        return current_domain;
    }
    if (!copy_bounded(current_domain, sizeof(current_domain), domain_name)) {
        return NULL;
    }
    return current_domain;
}

char *bindtextdomain(const char *domain_name, const char *directory_name) {
    if (!domain_name || domain_name[0] == '\0') {
        return NULL;
    }
    binding_t *binding = binding_for(domain_name, directory_name,
                                     DIRECTORY_MAX);
    if (!binding) {
        return NULL;
    }
    if (!directory_name) {
        return binding->has_directory ? binding->directory : NULL;
    }
    if (!copy_bounded(binding->directory, sizeof(binding->directory),
                      directory_name)) {
        return NULL;
    }
    binding->has_directory = 1;
    return binding->directory;
}

char *bind_textdomain_codeset(const char *domain_name, const char *codeset) {
    if (!domain_name || domain_name[0] == '\0') {
        return NULL;
    }
    binding_t *binding = binding_for(domain_name, codeset, CODESET_MAX);
    if (!binding) {
        return NULL;
    }
    if (!codeset) {
        return binding->has_codeset ? binding->codeset : NULL;
    }
    if (!copy_bounded(binding->codeset, sizeof(binding->codeset), codeset)) {
        return NULL;
    }
    binding->has_codeset = 1;
    return binding->codeset;
}
