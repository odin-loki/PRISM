// Model trace of std::priority_queue::operator<. libstdc++ has no
// priority_queue comparison operators and no <priority_queue> header, so
// this file is compiled only against the model. The walk is the heap index
// in its existing order. It does not sort and it does not use the
// comparator. Two empty queues are not less and do not load the root. An
// empty queue is less than a non-empty queue, and that path does not call
// T's operator<.
#include <cstdio>
#include <priority_queue>

// The model header declares this and calls it when the index grows.
extern "C" void __prism_memcpy(void* d, const void* s, unsigned long n, int) { __builtin_memmove(d, s, n); }

struct IntSeq {
    using value_type = int;
};

struct Greater {
    bool operator()(const int& a, const int& b) const { return a > b; }
};

// No default constructor. operator< counts reads. A negative value past the
// deciding index must not be read.
struct MemLt {
    int v;
    static int calls;
    static int poison;
    explicit MemLt(int x) : v(x) {}
    MemLt(const MemLt&) = delete;
    MemLt& operator=(const MemLt&) = delete;
    bool operator<(const MemLt& o) const {
        ++calls;
        if (v < 0 || o.v < 0) ++poison;
        return v < o.v;
    }
};

int MemLt::calls = 0;
int MemLt::poison = 0;

// Stored only. No operator<: the queue comparison is a function template and
// is not instantiated for this type.
struct NoLt {
    explicit NoLt(int) {}
    NoLt(const NoLt&) = delete;
};

static int fail(const char* what) {
    std::fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

static int less_ints() {
    std::priority_queue<int> empty_a;
    std::priority_queue<int> empty_b;
    const std::priority_queue<int> cempty_a;
    const std::priority_queue<int> cempty_b;
    const int empty = empty_a < empty_b;
    const int cempty = cempty_a < cempty_b;

    std::priority_queue<int> one;
    one.push(1);
    std::priority_queue<int> two;
    two.push(2);
    const int el = empty_a < one;
    const int le = one < empty_a;
    const int head = one < two;
    const int hd = two < one;
    const int self = one < one;

    std::priority_queue<int> store_a;
    const int seq_a[] = {1, 4, 3, 2};
    for (int v : seq_a) store_a.push(v);
    std::priority_queue<int> store_b;
    const int seq_b[] = {2, 1, 3, 4};
    for (int v : seq_b) store_b.push(v);
    std::priority_queue<int> store_same;
    for (int v : seq_a) store_same.push(v);
    const int same = store_a < store_same;
    const int store = store_a < store_b;
    const int storer = store_b < store_a;

    std::priority_queue<int> prefix;
    prefix.push(4);
    prefix.push(2);
    const int sh = prefix < store_a;
    const int lng = store_a < prefix;

    std::priority_queue<int, IntSeq, Greater> mina;
    const int min_a[] = {3, 1, 4, 2};
    for (int v : min_a) mina.push(v);
    std::priority_queue<int, IntSeq, Greater> minb;
    const int min_b[] = {4, 2, 3, 1};
    for (int v : min_b) minb.push(v);
    const int minlt = minb < mina;
    const int minrev = mina < minb;

    std::priority_queue<int> moved_src;
    moved_src.push(5);
    std::priority_queue<int> moved_dst = std::move(moved_src);
    const int moved = moved_src < moved_dst;
    const int moved_back = moved_dst < moved_src;

    std::priority_queue<MemLt> popped;
    popped.emplace(7);
    popped.pop();
    std::priority_queue<MemLt> fresh;
    MemLt::calls = 0;
    MemLt::poison = 0;
    const int popped_lt = popped < fresh;
    const int popped_calls = MemLt::calls;
    std::priority_queue<MemLt> live;
    live.emplace(3);
    MemLt::calls = 0;
    MemLt::poison = 0;
    const int plive = popped < live;
    const int lpop = live < popped;
    const int plive_calls = MemLt::calls;
    const int plive_poison = MemLt::poison;

    std::printf(
        "lt empty=%d el=%d le=%d head=%d hd=%d self=%d same=%d store=%d storer=%d short=%d long=%d minlt=%d minrev=%d "
        "cempty=%d moved=%d movedback=%d popped=%d plive=%d lpop=%d pcalls=%d lcalls=%d ppoison=%d\n",
        empty, el, le, head, hd, self, same, store, storer, sh, lng, minlt, minrev, cempty, moved, moved_back,
        popped_lt, plive, lpop, popped_calls, plive_calls, plive_poison);

    if (empty != 0 || cempty != 0) return fail("empty");
    if (el != 1) return fail("el");
    if (le != 0) return fail("le");
    if (head != 1 || hd != 0 || self != 0) return fail("head");
    if (same != 0) return fail("same");
    // Both heaps hold {1,2,3,4}. Index order differs, so this is less.
    // A sort of the values would compare them equal.
    if (store != 1 || storer != 0) return fail("store");
    if (sh != 1 || lng != 0) return fail("short");
    if (minlt != 1 || minrev != 0) return fail("min");
    if (moved != 1 || moved_back != 0) return fail("moved");
    if (popped_lt != 0 || plive != 1 || lpop != 0) return fail("popped");
    if (popped_calls != 0 || plive_calls != 0 || plive_poison != 0) return fail("popped calls");
    return 0;
}

static int less_mem() {
    std::priority_queue<MemLt> z1;
    std::priority_queue<MemLt> z2;
    MemLt::calls = 0;
    MemLt::poison = 0;
    const int zero = z1 < z2;
    const int calls_zero = MemLt::calls;

    std::priority_queue<MemLt> er;
    er.emplace(1);
    MemLt::calls = 0;
    MemLt::poison = 0;
    const int el = z1 < er;
    const int calls_el = MemLt::calls;
    MemLt::calls = 0;
    const int le = er < z1;
    const int calls_le = MemLt::calls;

    std::priority_queue<MemLt> hl;
    hl.emplace(1);
    hl.emplace(-1);
    std::priority_queue<MemLt> hr;
    hr.emplace(2);
    hr.emplace(-1);
    MemLt::calls = 0;
    MemLt::poison = 0;
    const int mhead = hl < hr;
    const int calls_head = MemLt::calls;
    const int poison_head = MemLt::poison;

    std::priority_queue<MemLt> gl;
    gl.emplace(2);
    gl.emplace(-1);
    std::priority_queue<MemLt> gr;
    gr.emplace(1);
    gr.emplace(-1);
    MemLt::calls = 0;
    MemLt::poison = 0;
    const int mgt = gl < gr;
    const int calls_gt = MemLt::calls;
    const int poison_gt = MemLt::poison;

    std::priority_queue<MemLt> ml;
    ml.emplace(4);
    ml.emplace(1);
    ml.emplace(-1);
    std::priority_queue<MemLt> mr;
    mr.emplace(4);
    mr.emplace(2);
    mr.emplace(-1);
    MemLt::calls = 0;
    MemLt::poison = 0;
    const int mmid = ml < mr;
    const int calls_mid = MemLt::calls;
    const int poison_mid = MemLt::poison;

    std::priority_queue<MemLt> sl;
    sl.emplace(4);
    sl.emplace(1);
    std::priority_queue<MemLt> sr;
    sr.emplace(4);
    sr.emplace(1);
    sr.emplace(-1);
    MemLt::calls = 0;
    MemLt::poison = 0;
    const int mshort = sl < sr;
    const int calls_short = MemLt::calls;
    const int poison_short = MemLt::poison;

    std::priority_queue<NoLt> nolt;

    std::printf(
        "mem zero=%d el=%d le=%d head=%d gt=%d mid=%d short=%d calls zero=%d el=%d le=%d head=%d hpoison=%d "
        "gt=%d gpoison=%d mid=%d mpoison=%d short=%d spoison=%d nolt=%zu\n",
        zero, el, le, mhead, mgt, mmid, mshort, calls_zero, calls_el, calls_le, calls_head, poison_head, calls_gt,
        poison_gt, calls_mid, poison_mid, calls_short, poison_short, nolt.size());

    if (zero != 0 || calls_zero != 0) return fail("zero");
    if (el != 1 || calls_el != 0) return fail("el");
    if (le != 0 || calls_le != 0) return fail("le");
    if (mhead != 1 || calls_head != 1 || poison_head != 0) return fail("head");
    if (mgt != 0 || calls_gt != 2 || poison_gt != 0) return fail("gt");
    if (mmid != 1 || calls_mid != 3 || poison_mid != 0) return fail("mid");
    if (mshort != 1 || calls_short != 4 || poison_short != 0) return fail("short");
    if (nolt.size() != 0) return fail("nolt");
    return 0;
}

int main() {
    if (int rc = less_ints()) return rc;
    if (int rc = less_mem()) return rc;
    return 0;
}
