/* Array stack and array queue (head index plus count, wrapped with a
 * compare instead of a modulo). */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
int __VERIFIER_nondet_int(void);
/* --- end prelude --- */

#define STK_CAP 8

struct stack {
    int top; /* number of elements */
    int v[STK_CAP];
};

/*@ requires \valid(s);
    requires 0 <= s->top <= STK_CAP;
    assigns s->top, s->v[0..STK_CAP-1];
    ensures \result == 0 <==> \old(s->top) < STK_CAP;
    ensures \result == 0 ==> s->top == \old(s->top) + 1 && s->v[\old(s->top)] == x;
    ensures \result != 0 ==> s->top == \old(s->top);
    ensures 0 <= s->top <= STK_CAP;
*/
int stack_push(struct stack *s, int x) {
    if (s->top >= STK_CAP) return -1;
    s->v[s->top++] = x;
    return 0;
}

/*@ requires \valid(s) && \valid(out);
    requires 0 <= s->top <= STK_CAP;
    assigns s->top, *out;
    ensures \result == 0 <==> \old(s->top) > 0;
    ensures \result == 0 ==> s->top == \old(s->top) - 1 && *out == s->v[s->top];
    ensures \result != 0 ==> s->top == \old(s->top) && *out == \old(*out);
    ensures 0 <= s->top <= STK_CAP;
*/
int stack_pop(struct stack *s, int *out) {
    if (s->top <= 0) return -1;
    *out = s->v[--s->top];
    return 0;
}

/* LIFO: push a then b, pop gives b then a */
void stack_lifo_contract(int a, int b, int depth) {
    struct stack s;
    int x = 0, y = 0;
    if (depth < 0 || depth > STK_CAP - 2) return;
    s.top = depth;
    assert(stack_push(&s, a) == 0 && stack_push(&s, b) == 0);
    assert(stack_pop(&s, &x) == 0 && stack_pop(&s, &y) == 0);
    assert(x == b && y == a && s.top == depth);
}

#define Q_CAP 5

struct queue {
    int head, count;
    int v[Q_CAP];
};

/*@ requires \valid(q);
    requires 0 <= q->head < Q_CAP && 0 <= q->count <= Q_CAP;
    assigns q->count, q->v[0..Q_CAP-1];
    ensures \result == 0 <==> \old(q->count) < Q_CAP;
    ensures \result == 0 ==> q->count == \old(q->count) + 1;
    ensures 0 <= q->head < Q_CAP && 0 <= q->count <= Q_CAP && q->head == \old(q->head);
*/
int queue_put(struct queue *q, int x) {
    if (q->count >= Q_CAP) return -1;
    int t = q->head + q->count;
    if (t >= Q_CAP) t -= Q_CAP;
    q->v[t] = x;
    q->count++;
    return 0;
}

/*@ requires \valid(q) && \valid(out);
    requires 0 <= q->head < Q_CAP && 0 <= q->count <= Q_CAP;
    assigns q->head, q->count, *out;
    ensures \result == 0 <==> \old(q->count) > 0;
    ensures \result == 0 ==> *out == \old(q->v[q->head]) && q->count == \old(q->count) - 1;
    ensures 0 <= q->head < Q_CAP && 0 <= q->count <= Q_CAP;
*/
int queue_get(struct queue *q, int *out) {
    if (q->count <= 0) return -1;
    *out = q->v[q->head];
    q->head = q->head + 1 == Q_CAP ? 0 : q->head + 1;
    q->count--;
    return 0;
}

/* FIFO from any start position: put a, b; get a, b */
void queue_fifo_contract(int a, int b, int head, int count) {
    struct queue q;
    int x = 0, y = 0;
    if (head < 0 || head >= Q_CAP || count < 0 || count > Q_CAP - 2) return;
    q.head = head;
    q.count = 0;
    for (int i = 0; i < count; i++) queue_put(&q, 0);
    for (int i = 0; i < count; i++) queue_get(&q, &x);
    assert(queue_put(&q, a) == 0 && queue_put(&q, b) == 0);
    assert(queue_get(&q, &x) == 0 && queue_get(&q, &y) == 0);
    assert(x == a && y == b && q.count == 0);
}

/* Planted twin: pop of an empty stack reads v[-1]. The array is a local, not
 * struct stack's v: v is the struct's last field, which the pir stage exempts
 * from the per-array bound (flexible array / struct hack, docs/PIR.md), so
 * v[-1] there only reads s.top and is inside the object. */
int stack_pop_unchecked_bad(int depth) {
    int v[STK_CAP];
    if (depth < 0 || depth > STK_CAP) return 0;
    for (int i = 0; i < STK_CAP; i++) v[i] = i;
    int top = depth;
    return v[--top];
}
