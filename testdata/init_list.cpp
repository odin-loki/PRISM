int init_list_bad(void) {
    const int *p = initializer_list<int>{1, 2, 3}.begin();
    return *p;
}

int init_list_ok(void) {
    vector<int> v;
    v.push_back(1);
    v.push_back(2);
    v.push_back(3);
    return 1;
}
