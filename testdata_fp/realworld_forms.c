/* Reduced from jsmn (MIT), cJSON (MIT) and jsmn's example/jsondump.c,
 * docs/EVALUATION.md. An object-like export macro before the return type
 * (`JSMN_API int f(`), a function-like one wrapping it
 * (`CJSON_PUBLIC(char *) f(`), and local arrays sized by constant macros. */
#include <stdio.h>
#include <string.h>

#define JSMN_API extern
#define CJSON_PUBLIC(type) type
#define NAME_MAX_LEN 32

typedef int cJSON_bool;

JSMN_API int jsmn_count(const char *js, unsigned len) {
    return len > 0 && js[0] == '{';
}

CJSON_PUBLIC(const char *) version_string(void) {
    static char v[16];
    snprintf(v, sizeof(v), "%d.%d", 1, 7);
    return v;
}

CJSON_PUBLIC(cJSON_bool) is_empty(const char *s) {
    return s == NULL || s[0] == '\0';
}

size_t fixed_buffers(FILE *in) {
    char buf[BUFSIZ];
    char name[NAME_MAX_LEN * 2 + 1];
    char word[sizeof(int) * 4];
    size_t n = fread(buf, 1, sizeof(buf), in);
    name[0] = word[0] = '\0';
    return n + strlen(name) + strlen(word);
}
