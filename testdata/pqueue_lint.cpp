void pqueue_bad(void) {
    std::priority_queue<int> pq;
    pq.top();
}
void pqueue_ok(void) {
    std::priority_queue<int> pq;
    if (pq.empty()) return;
    pq.top();
}
