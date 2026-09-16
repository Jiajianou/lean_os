#pragma once

#ifdef __cplusplus
extern "C" {
#endif

char *gettext(const char *message_id);
char *dgettext(const char *domain_name, const char *message_id);
char *dcgettext(const char *domain_name, const char *message_id, int category);

char *ngettext(const char *message_id, const char *message_id_plural,
               unsigned long n);
char *dngettext(const char *domain_name, const char *message_id,
                const char *message_id_plural, unsigned long n);
char *dcngettext(const char *domain_name, const char *message_id,
                 const char *message_id_plural, unsigned long n, int category);

char *textdomain(const char *domain_name);
char *bindtextdomain(const char *domain_name, const char *directory_name);
char *bind_textdomain_codeset(const char *domain_name, const char *codeset);

#ifdef __cplusplus
}
#endif
