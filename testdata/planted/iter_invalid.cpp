struct Vec {
    int *begin();
    int *end();
    void push_back(int);
};

void iter_bad(Vec &v) {
    int *it = v.begin();
    v.push_back(1);
    *it = 0;
}

void iter_ok(Vec &v) {
    v.push_back(1);
}
