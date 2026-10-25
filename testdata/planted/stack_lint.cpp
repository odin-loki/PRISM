void stack_bad(void) {
    std::stack<int> st;
    st.top();
}
void stack_ok(void) {
    std::stack<int> st;
    if (st.empty()) return;
    st.top();
}
