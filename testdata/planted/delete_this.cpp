struct Node {
    Node *next;
};

void delete_this_bad(void) {
    delete this;
}

void delete_this_ok(Node *p) {
    delete p;
}

void delete_this_array_ok(Node *p) {
    delete[] p;
}
