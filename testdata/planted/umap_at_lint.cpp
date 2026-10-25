void umap_at_bad(int i) {
    std::unordered_map<int,int> m;
    m.at(i);
}

void umap_at_ok(int i) {
    std::unordered_map<int,int> m;
    if (m.count(i)) m.at(i);
}
