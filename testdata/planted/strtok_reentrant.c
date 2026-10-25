char *strtok(char *s, const char *sep);
char *strtok_r(char *s, const char *sep, char **last);
char *strtok_s(char *s, const char *sep, char **last);

void strtok_bad(char *s) {
    strtok(s, ",");
}

void strtok_ok(char *s, char **last) {
    strtok_r(s, ",", last);
}

void strtok_s_ok(char *s, char **last) {
    strtok_s(s, ",", last);
}
