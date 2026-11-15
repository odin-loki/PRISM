/* Linked lists: the kernel's circular doubly linked list (include/linux/list.h
 * shapes: __list_add, list_add, list_add_tail, __list_del, list_del,
 * list_empty, list_is_head), container_of, and a singly linked stack. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
#include <stddef.h>
int __VERIFIER_nondet_int(void);
unsigned __VERIFIER_nondet_uint(void);
/* --- end prelude --- */

struct list_head {
    struct list_head *next, *prev;
};

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/*@ requires \valid(list);
    assigns list->next, list->prev;
    ensures list->next == list && list->prev == list;
*/
void INIT_LIST_HEAD(struct list_head *list) {
    list->next = list;
    list->prev = list;
}

/* Insert node between two consecutive nodes. The pointer parameters are
 * separate objects in this proof (pir's \separated assumption); prev == next
 * (an empty list) is covered by list_add_contract below. */
/*@ requires \valid(node) && \valid(prev) && \valid(next);
    requires prev->next == next && next->prev == prev;
    assigns node->next, node->prev, prev->next, next->prev;
    ensures prev->next == node && node->prev == prev;
    ensures node->next == next && next->prev == node;
*/
void __list_add(struct list_head *node, struct list_head *prev, struct list_head *next) {
    next->prev = node;
    node->next = next;
    node->prev = prev;
    prev->next = node;
}

/* list_add / list_add_tail read head->next / head->prev, objects reached
 * through memory: their contracts are proved by the harnesses below on
 * lists of 0..3 nodes (\valid of a field path is not a pir binding). */
/*@ requires \valid(node) && \valid(head);
    requires \valid(head->next) && head->next->prev == head;
    ensures head->next == node && node->prev == head && node->next == \old(head->next);
*/
void list_add(struct list_head *node, struct list_head *head) { __list_add(node, head, head->next); }

/*@ requires \valid(node) && \valid(head);
    requires \valid(head->prev) && head->prev->next == head;
    ensures head->prev == node && node->next == head && node->prev == \old(head->prev);
*/
void list_add_tail(struct list_head *node, struct list_head *head) { __list_add(node, head->prev, head); }

/*@ requires \valid(prev) && \valid(next);
    assigns prev->next, next->prev;
    ensures prev->next == next && next->prev == prev;
*/
void __list_del(struct list_head *prev, struct list_head *next) {
    next->prev = prev;
    prev->next = next;
}

/*@ requires \valid(node);
    requires \valid(node->prev) && \valid(node->next);
    ensures node->next == \null && node->prev == \null;
*/
void list_del(struct list_head *node) {
    __list_del(node->prev, node->next);
    node->next = 0; /* the kernel writes LIST_POISON1/2 */
    node->prev = 0;
}

/*@ requires \valid_read(head);
    assigns \nothing;
    ensures \result == (head->next == head);
*/
int list_empty(const struct list_head *head) { return head->next == head; }

/*@ requires \valid_read(list) && \valid_read(head);
    assigns \nothing;
    ensures \result == (list == head);
*/
int list_is_head(const struct list_head *list, const struct list_head *head) { return list == head; }

/* ---- harnesses: lists of k = 0..3 nodes in a local array ---- */

#define LMAX 3

/* n[0] is the head; n[1..k] the nodes in order; circular. */
/*@ requires \valid(n + (0..3)); requires 0 <= k <= 3; */
static void list_build(struct list_head *n, int k) {
    INIT_LIST_HEAD(&n[0]);
    for (int i = 1; i <= k; i++) list_add_tail(&n[i], &n[0]);
}

/* well-formed: k steps forward from the head return to it, every node's
 * prev is the node before it, every link stays inside n[0..3] (checked
 * before it is followed, so this is safe on any contents of n) */
/*@ requires \valid_read(n + (0..3)); requires 0 <= k <= 4; */
static int list_ok(const struct list_head *n, int k) {
    int i = 0;
    for (int s = 0; s <= k; s++) {
        int nx = -1;
        for (int j = 0; j < 4; j++)
            if (n[i].next == &n[j]) nx = j;
        if (nx < 0 || n[nx].prev != &n[i]) return 0;
        i = nx;
        if ((i == 0) != (s == k)) return 0;
    }
    return 1;
}

void list_add_tail_contract(int k) {
    struct list_head n[LMAX + 1];
    if (k < 0 || k > LMAX) return;
    list_build(n, k);
    assert(list_ok(&n[0], k));
    for (int i = 1; i <= k; i++) assert(n[i].prev == &n[i - 1]);
    assert(n[0].prev == &n[k]);
}

void list_add_contract(int k) {
    struct list_head n[LMAX + 1];
    if (k < 0 || k >= LMAX) return;
    list_build(n, k);
    struct list_head *first = n[0].next, *e = &n[k + 1];
    list_add(e, &n[0]);
    assert(n[0].next == e && e->prev == &n[0] && e->next == first);
    assert(list_ok(&n[0], k + 1));
}

void list_del_contract(int k, int j) {
    struct list_head n[LMAX + 1];
    if (k < 1 || k > LMAX || j < 1 || j > k) return;
    list_build(n, k);
    struct list_head *before = n[j].prev, *after = n[j].next;
    list_del(&n[j]);
    assert(before->next == after && after->prev == before);
    assert(n[j].next == 0 && n[j].prev == 0);
    assert(list_ok(&n[0], k - 1));
}

void list_empty_contract(int k) {
    struct list_head n[LMAX + 1];
    if (k < 0 || k > LMAX) return;
    list_build(n, k);
    assert(list_empty(&n[0]) == (k == 0));
    assert(list_is_head(n[0].prev->next, &n[0]));
}

struct item {
    int key;
    struct list_head link;
};

void container_of_contract(int key) {
    struct item it;
    it.key = key;
    struct list_head *l = &it.link;
    struct item *back = container_of(l, struct item, link);
    assert(back == &it && back->key == key);
}

/* Planted twin: deleting the only node and then reading its successor's
 * prev through the cleared pointer (NULL dereference). */
void list_del_use_bad(void) {
    struct list_head n[2];
    list_build(n, 1);
    list_del(&n[1]);
    struct list_head *p = n[1].next;
    assert(p->prev == &n[0]);
}

/* Planted twin of a contract: "list_add_tail makes the node the first". */
void list_add_tail_first_bad(int k) {
    struct list_head n[LMAX + 1], e;
    if (k < 1 || k >= LMAX) return;
    list_build(n, k);
    list_add_tail(&e, &n[0]);
    assert(n[0].next == &e);
}

/* ---- singly linked stack ---- */

struct snode {
    struct snode *next;
    int v;
};

/*@ requires \valid(head) && \valid(n);
    assigns *head, n->next;
    ensures *head == n && n->next == \old(*head);
*/
void slist_push(struct snode **head, struct snode *n) {
    n->next = *head;
    *head = n;
}

/*@ requires \valid(head);
    requires *head == \null || \valid(*head);
    ensures \result == \old(*head);
    ensures \result != \null ==> *head == \result->next;
*/
struct snode *slist_pop(struct snode **head) {
    struct snode *n = *head;
    if (n) *head = n->next;
    return n;
}

void slist_contract(int a, int b) {
    struct snode x, y, *head = 0;
    x.v = a;
    y.v = b;
    slist_push(&head, &x);
    slist_push(&head, &y);
    struct snode *p = slist_pop(&head);
    struct snode *q = slist_pop(&head);
    struct snode *r = slist_pop(&head);
    assert(p == &y && p->v == b && q == &x && q->v == a && r == 0 && head == 0);
}
