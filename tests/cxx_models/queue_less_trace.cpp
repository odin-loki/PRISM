// Differential trace of std::queue::operator<. Compiled twice: once against
// libstdc++ and once with only the model header ahead of the system headers
// as <queue>. Element order is front to back. Two empty queues are not less
// and do not call T's operator<. An empty queue is less than a non-empty
// queue and does not call T's operator< either. A later element is not read
// once an earlier element decides, and an element past the shorter queue is
// not read.
#include <cstdio>
#include <queue>

struct MemLt {
    int v;
    static int calls;
    static int poison;
    explicit MemLt(int x) : v(x) {}
    MemLt(const MemLt& o) : v(o.v) {}
    MemLt() = delete;
    bool operator<(const MemLt& o) const {
        ++calls;
        if (v < 0 || o.v < 0) ++poison;
        return v < o.v;
    }
};

int MemLt::calls = 0;
int MemLt::poison = 0;

struct NeverLt {
    int v;
    static int calls;
    explicit NeverLt(int x) : v(x) {}
    NeverLt(const NeverLt& o) : v(o.v) {}
    NeverLt() = delete;
    bool operator<(const NeverLt&) const {
        ++calls;
        return false;
    }
};

int NeverLt::calls = 0;

// Stored and not compared. No operator< and no default constructor: the
// queue's operator< is a function template and is not instantiated here.
struct NoLt {
    int v;
    explicit NoLt(int x) : v(x) {}
    NoLt(const NoLt& o) : v(o.v) {}
    NoLt() = delete;
};

struct Rec {
    int v;
    explicit Rec(int x) : v(x) {}
    std::queue<Rec> kids;
};

template <typename T, int N>
std::queue<T> make(const int (&xs)[N]) {
    std::queue<T> q;
    for (int x : xs) q.push(T(x));
    return q;
}

static int fail(const char* what) {
    std::fprintf(stderr, "FAIL %s\n", what);
    return 1;
}

static int less_ints() {
    std::queue<int> empty_a;
    std::queue<int> empty_b;
    const int ee = empty_a < empty_b;

    std::queue<int> one;
    one.push(1);
    const int el = empty_a < one;
    const int le = one < empty_b;

    const int same_xs[] = {1, 2, 3};
    const int mid_xs[] = {1, 0, 3};
    const int head_xs[] = {0, 2, 3};
    const int tail_xs[] = {1, 2, 4};
    const int early_xs[] = {2, 0, 0};
    const int early_r_xs[] = {1, 9, 9};
    const int one_xs[] = {5};
    const int one_hi_xs[] = {6};
    const int pre_xs[] = {1};
    const int pre_long_xs[] = {1, 2};
    const int front_xs[] = {1, 9};
    const int front_r_xs[] = {2, 0};
    const int ca_xs[] = {4, 5};
    const int cc_xs[] = {4, 6};

    const auto a = make<int>(same_xs);
    const auto same = make<int>(same_xs);
    const auto mid = make<int>(mid_xs);
    const auto head = make<int>(head_xs);
    const auto tail = make<int>(tail_xs);
    const auto early = make<int>(early_xs);
    const auto early_r = make<int>(early_r_xs);
    const auto o5 = make<int>(one_xs);
    const auto o6 = make<int>(one_hi_xs);
    const auto o5b = make<int>(one_xs);
    const auto pre = make<int>(pre_xs);
    const auto pre_long = make<int>(pre_long_xs);
    const auto fr = make<int>(front_xs);
    const auto fr_r = make<int>(front_r_xs);
    const auto ca = make<int>(ca_xs);
    const auto cb = make<int>(ca_xs);
    const auto cc = make<int>(cc_xs);

    std::queue<int> live;
    live.push(1);
    live.push(2);
    std::printf("ops front=%d back=%d size=%zu empty=%d\n", live.front(), live.back(), live.size(),
                int(live.empty()));
    live.pop();
    std::printf("ops pop front=%d back=%d size=%zu empty=%d\n", live.front(), live.back(), live.size(),
                int(live.empty()));

    std::printf(
        "lt empty=%d el=%d le=%d same=%d mid=%d head=%d tail=%d gt=%d early=%d one=%d oned=%d oneeq=%d self=%d "
        "prefix=%d longer=%d front=%d csame=%d cdiff=%d\n",
        ee, el, le, int(a < same), int(mid < a), int(head < a), int(a < tail), int(tail < a), int(early < early_r),
        int(o5 < o6), int(o6 < o5), int(o5 < o5b), int(a < a), int(pre < pre_long), int(pre_long < pre),
        int(fr < fr_r), int(ca < cb), int(ca < cc));

    if (ee != 0) return fail("empty");
    if (el != 1) return fail("empty<nonempty");
    if (le != 0) return fail("nonempty<empty");
    if ((a < same) != 0) return fail("same");
    if ((mid < a) != 1) return fail("mid");
    if ((head < a) != 1) return fail("head");
    if ((a < tail) != 1) return fail("tail");
    if ((tail < a) != 0) return fail("gt");
    if ((early < early_r) != 0) return fail("early");
    if ((o5 < o6) != 1) return fail("one");
    if ((o6 < o5) != 0) return fail("oned");
    if ((o5 < o5b) != 0) return fail("oneeq");
    if ((a < a) != 0) return fail("self");
    if ((pre < pre_long) != 1) return fail("prefix");
    if ((pre_long < pre) != 0) return fail("longer");
    if ((fr < fr_r) != 1) return fail("front");
    if ((ca < cb) != 0) return fail("csame");
    if ((ca < cc) != 1) return fail("cdiff");
    return 0;
}

static int count_less(const std::queue<MemLt>& x, const std::queue<MemLt>& y, int& calls, int& poison) {
    MemLt::calls = 0;
    MemLt::poison = 0;
    const bool r = x < y;
    calls = MemLt::calls;
    poison = MemLt::poison;
    return int(r);
}

static int less_mem() {
    const int same_xs[] = {1, 2, 3};
    const int mid_xs[] = {1, 0, 3};
    const int head_xs[] = {0, 2, 3};
    const int tail_xs[] = {1, 2, 4};
    const int early_xs[] = {2, 0, 0};
    const int early_r_xs[] = {1, 9, 9};
    const int poison_xs[] = {1, 2, -1};
    const int poison_r_xs[] = {1, 9, -1};
    const int pgt_xs[] = {2, -1, -1};
    const int pgt_r_xs[] = {1, 0, 0};
    const int pre_xs[] = {1, -1};
    const int pre_r_xs[] = {1};
    const int short_xs[] = {1};
    const int short_r_xs[] = {1, -1};
    const int front_xs[] = {1, -1};
    const int front_r_xs[] = {2, -1};

    const auto same_a = make<MemLt>(same_xs);
    const auto same_b = make<MemLt>(same_xs);
    const auto mid = make<MemLt>(mid_xs);
    const auto head = make<MemLt>(head_xs);
    const auto tail = make<MemLt>(tail_xs);
    const auto early = make<MemLt>(early_xs);
    const auto early_r = make<MemLt>(early_r_xs);
    const auto poison_a = make<MemLt>(poison_xs);
    const auto poison_b = make<MemLt>(poison_r_xs);
    const auto pgt_a = make<MemLt>(pgt_xs);
    const auto pgt_b = make<MemLt>(pgt_r_xs);
    const auto pre_a = make<MemLt>(pre_xs);
    const auto pre_b = make<MemLt>(pre_r_xs);
    const auto short_a = make<MemLt>(short_xs);
    const auto short_b = make<MemLt>(short_r_xs);
    const auto front_a = make<MemLt>(front_xs);
    const auto front_b = make<MemLt>(front_r_xs);
    const std::queue<MemLt> zero_a;
    const std::queue<MemLt> zero_b;
    std::queue<MemLt> one;
    one.push(MemLt(1));

    int c_same = 0, p_same = 0, c_mid = 0, p_mid = 0, c_head = 0, p_head = 0, c_tail = 0, p_tail = 0;
    int c_gt = 0, p_gt = 0, c_early = 0, p_early = 0, c_poison = 0, p_poison = 0, c_pgt = 0, p_pgt = 0;
    int c_pre = 0, p_pre = 0, c_short = 0, p_short = 0, c_front = 0, p_front = 0;
    int c_zero = 0, p_zero = 0, c_el = 0, p_el = 0, c_le = 0, p_le = 0;
    const int same = count_less(same_a, same_b, c_same, p_same);
    const int mid_lt = count_less(mid, same_a, c_mid, p_mid);
    const int head_lt = count_less(head, same_a, c_head, p_head);
    const int tail_lt = count_less(same_a, tail, c_tail, p_tail);
    const int gt = count_less(tail, same_a, c_gt, p_gt);
    const int early_lt = count_less(early, early_r, c_early, p_early);
    const int poison = count_less(poison_a, poison_b, c_poison, p_poison);
    const int pgt = count_less(pgt_a, pgt_b, c_pgt, p_pgt);
    const int prefix = count_less(pre_a, pre_b, c_pre, p_pre);
    const int shorter = count_less(short_a, short_b, c_short, p_short);
    const int front = count_less(front_a, front_b, c_front, p_front);
    const int zero = count_less(zero_a, zero_b, c_zero, p_zero);
    const int el = count_less(zero_a, one, c_el, p_el);
    const int le = count_less(one, zero_b, c_le, p_le);

    const int never_xs[] = {1, 2, 3};
    const int never_r_xs[] = {9, 8, 7};
    const auto na = make<NeverLt>(never_xs);
    const auto nb = make<NeverLt>(never_r_xs);
    NeverLt::calls = 0;
    const int never = na < nb;
    const int c_never = NeverLt::calls;
    const std::queue<NeverLt> nz;
    const std::queue<NeverLt> nw;
    NeverLt::calls = 0;
    const int nzero = nz < nw;
    const int c_nzero = NeverLt::calls;

    std::queue<NoLt> nolt;
    nolt.emplace(3);
    nolt.emplace(4);
    std::queue<Rec> rec;
    rec.emplace(1);
    rec.front().kids.emplace(2);

    std::printf(
        "ltm same=%d mid=%d head=%d tail=%d gt=%d early=%d zero=%d el=%d le=%d poison=%d pgt=%d prefix=%d short=%d "
        "front=%d never=%d nzero=%d calls same=%d mid=%d head=%d tail=%d gt=%d early=%d zero=%d el=%d le=%d "
        "poison=%d pread=%d pgt=%d pgread=%d prefix=%d pread=%d short=%d sread=%d front=%d fread=%d never=%d "
        "nzero=%d nolt=%d %d %zu rec=%d %zu\n",
        same, mid_lt, head_lt, tail_lt, gt, early_lt, zero, el, le, poison, pgt, prefix, shorter, front, never,
        nzero, c_same, c_mid, c_head, c_tail, c_gt, c_early, c_zero, c_el, c_le, c_poison, p_poison, c_pgt, p_pgt,
        c_pre, p_pre, c_short, p_short, c_front, p_front, c_never, c_nzero, nolt.front().v, nolt.back().v, nolt.size(),
        rec.front().v, rec.front().kids.size());

    if (same != 0 || c_same != 6) return fail("same");
    if (mid_lt != 1 || c_mid != 3) return fail("mid");
    if (head_lt != 1 || c_head != 1) return fail("head");
    if (tail_lt != 1 || c_tail != 5) return fail("tail");
    if (gt != 0 || c_gt != 6) return fail("gt");
    if (early_lt != 0 || c_early != 2) return fail("early");
    if (zero != 0 || c_zero != 0 || p_zero != 0) return fail("zero");
    if (el != 1 || c_el != 0 || p_el != 0) return fail("el");
    if (le != 0 || c_le != 0 || p_le != 0) return fail("le");
    if (poison != 1 || c_poison != 3 || p_poison != 0) return fail("poison");
    if (pgt != 0 || c_pgt != 2 || p_pgt != 0) return fail("pgt");
    if (prefix != 0 || c_pre != 2 || p_pre != 0) return fail("prefix");
    if (shorter != 1 || c_short != 2 || p_short != 0) return fail("short");
    if (front != 1 || c_front != 1 || p_front != 0) return fail("front");
    if (never != 0 || c_never != 6) return fail("never");
    if (nzero != 0 || c_nzero != 0) return fail("nzero");
    if (nolt.front().v != 3 || nolt.back().v != 4 || nolt.size() != 2) return fail("nolt");
    if (rec.front().v != 1 || rec.front().kids.size() != 1) return fail("rec");
    return 0;
}

int main() {
    if (int rc = less_ints()) return rc;
    if (int rc = less_mem()) return rc;
    return 0;
}
