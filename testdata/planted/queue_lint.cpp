void queue_bad(void) {
    std::queue<int> q;
    q.front();
}
void queue_ok(void) {
    std::queue<int> q;
    if (q.empty()) return;
    q.front();
}
