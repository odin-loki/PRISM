void inplace_vec_bad(void) {
    std::inplace_vector<int, 4> v;
    v.push_back(1);
}

void inplace_vec_ok(void) {
    std::inplace_vector<int, 4> v;
    if (v.size() < v.capacity())
        v.push_back(1);
}
