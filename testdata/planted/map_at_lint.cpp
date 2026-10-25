void map_at_bad(int i) {
    std::map<int,int> m;
    m.at(i);
}

void map_at_ok(int i) {
    std::map<int,int> m;
    if (m.count(i)) m.at(i);
}
