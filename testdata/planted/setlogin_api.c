int setlogin(const char *name);

void setlogin_bad(void) {
    setlogin("root");
}

void setlogin_ok(void) {
    if (setlogin("root") != 0)
        return;
}
