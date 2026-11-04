// Trace of std::flat_set. Include <flat_set> and use insert, contains, find,
// erase, and operator<. A second insert of the same key does not grow size.
// find of a missing key is end. erase of a missing key leaves size unchanged.
// operator< compares stored keys lexicographically in link order. Keys are
// linked in compare order, so that order is the sorted order libstdc++
// walks. Two empty sets are not less. An empty set is less than a non-empty
// set. A key past the shorter set is not read, and the comparison does not
// construct a key. operator> returns the existing operator< with the
// arguments swapped. Two empty sets are not greater. A non-empty set is
// greater than an empty set. That path does not load a key and does not
// construct one. Key operator> is not called. The lines above the less
// block are unchanged.
// tests/cpp/test_cxx_flat_set.cpp checks the model text; it does not shell
// out to this file.
#include <cstdio>
#include <flat_set>

// The models call __prism_check, the precondition intrinsic. libstdc++ never
// calls it. A failed check aborts so the model and libstdc++ runs still differ.
extern "C" __attribute__((weak)) void __prism_check(int ok, const char*, const char*) {
    if (!ok) __builtin_abort();
}

// No default constructor. operator< counts calls and a read of the sentinel
// 100, which sorts after 1 and 2. A constructed key counts too, so a
// comparison that builds T{} shows up as a constructor.
struct LtKey {
    int v;
    static int calls;
    static int poison;
    static int ctors;
    explicit LtKey(int x) : v(x) { ++ctors; }
    LtKey(const LtKey& o) : v(o.v) { ++ctors; }
    LtKey(LtKey&& o) noexcept : v(o.v) { ++ctors; }
    bool operator<(const LtKey& o) const {
        ++calls;
        if (v == 100 || o.v == 100)
            ++poison;
        return v < o.v;
    }
};

int LtKey::calls = 0;
int LtKey::poison = 0;
int LtKey::ctors = 0;

// Key operator< counts lt. Key operator> counts gt and compares the ints
// itself, so a set comparison that called key operator> increments gt.
struct OrdKey {
    int v;
    static int lt;
    static int gt;
    explicit OrdKey(int x) : v(x) {}
    OrdKey(const OrdKey& o) : v(o.v) {}
    OrdKey(OrdKey&& o) noexcept : v(o.v) {}
    bool operator<(const OrdKey& o) const {
        ++lt;
        return v < o.v;
    }
    bool operator>(const OrdKey& o) const {
        ++gt;
        return v > o.v;
    }
};

int OrdKey::lt = 0;
int OrdKey::gt = 0;

static void reset_ord() {
    OrdKey::lt = 0;
    OrdKey::gt = 0;
}

static void add_ord(std::flat_set<OrdKey>& s, const int* xs, int n) {
    for (int i = 0; i < n; ++i)
        s.insert(OrdKey(xs[i]));
}

static void reset_key() {
    LtKey::calls = 0;
    LtKey::poison = 0;
    LtKey::ctors = 0;
}

static void add_keys(std::flat_set<LtKey>& s, const int* xs, int n) {
    for (int i = 0; i < n; ++i)
        s.insert(LtKey(xs[i]));
}

int main() {
    std::flat_set<int> empty;
    std::printf("empty_eq %d\n", empty.begin() == empty.end() ? 1 : 0);
    std::printf("empty_size %d\n", static_cast<int>(empty.size()));

    std::flat_set<int> s;
    s.insert(2);
    s.insert(1);
    auto dup = s.insert(2);
    std::printf("dup_inserted %d size %d\n", dup.second ? 1 : 0, static_cast<int>(s.size()));
    std::printf("contains1 %d contains3 %d\n", s.contains(1) ? 1 : 0, s.contains(3) ? 1 : 0);
    std::printf("find3_end %d\n", s.find(3) == s.end() ? 1 : 0);
    std::printf("order");
    for (int k : s)
        std::printf(" %d", k);
    std::printf("\n");
    std::printf("erase_missing %d size %d\n", static_cast<int>(s.erase(3)), static_cast<int>(s.size()));
    std::printf("erase_present %d size %d\n", static_cast<int>(s.erase(2)), static_cast<int>(s.size()));
    std::printf("contains1 %d contains2 %d\n", s.contains(1) ? 1 : 0, s.contains(2) ? 1 : 0);

    std::flat_set<int> none;
    std::flat_set<int> one;
    one.insert(1);
    std::flat_set<int> two;
    two.insert(1);
    two.insert(2);
    std::flat_set<int> other;
    other.insert(1);
    other.insert(3);
    std::flat_set<int> three;
    three.insert(1);
    three.insert(2);
    three.insert(3);
    const std::flat_set<int> cone = one;
    std::printf(
        "lt empty %d short %d rev %d same %d mid %d gt %d prefix %d longer %d one %d self %d const %d\n",
        none < none ? 1 : 0, none < one ? 1 : 0, one < none ? 1 : 0, two < two ? 1 : 0, two < other ? 1 : 0,
        other < two ? 1 : 0, two < three ? 1 : 0, three < two ? 1 : 0, one < two ? 1 : 0, two < two ? 1 : 0,
        cone < one ? 1 : 0);

    std::flat_set<LtKey> kempty;
    std::flat_set<LtKey> kempty2;
    reset_key();
    const int kempty_lt = kempty < kempty2 ? 1 : 0;
    const int kempty_calls = LtKey::calls;
    const int kempty_ctors = LtKey::ctors;
    const int kempty_poison = LtKey::poison;

    const int k1[] = {1};
    std::flat_set<LtKey> kone;
    add_keys(kone, k1, 1);
    reset_key();
    const int kshort_lt = kempty < kone ? 1 : 0;
    const int kshort_calls = LtKey::calls;
    const int kshort_ctors = LtKey::ctors;
    reset_key();
    const int krev_lt = kone < kempty ? 1 : 0;
    const int krev_calls = LtKey::calls;
    const int krev_ctors = LtKey::ctors;

    const int k12[] = {1, 2};
    const int k12p[] = {1, 2, 100};
    const int k19p[] = {1, 9, 100};
    const int k1p[] = {1, 100};
    const int k2p[] = {2, 100};
    std::flat_set<LtKey> ka;
    std::flat_set<LtKey> kb;
    std::flat_set<LtKey> kmiss_set;
    std::flat_set<LtKey> kearly;
    std::flat_set<LtKey> kgt;
    add_keys(ka, k12, 2);
    add_keys(kb, k12p, 3);
    add_keys(kmiss_set, k19p, 3);
    add_keys(kearly, k1p, 2);
    add_keys(kgt, k2p, 2);
    reset_key();
    const int kprefix = ka < kb ? 1 : 0;
    const int kprefix_calls = LtKey::calls;
    const int kprefix_poison = LtKey::poison;
    const int kprefix_ctors = LtKey::ctors;
    reset_key();
    const int klonger = kb < ka ? 1 : 0;
    const int klonger_calls = LtKey::calls;
    const int klonger_poison = LtKey::poison;
    const int klonger_ctors = LtKey::ctors;
    reset_key();
    const int kmiss = kmiss_set < kb ? 1 : 0;
    const int kmiss_calls = LtKey::calls;
    const int kmiss_poison = LtKey::poison;
    reset_key();
    const int khead = kearly < kgt ? 1 : 0;
    const int khead_calls = LtKey::calls;
    const int khead_poison = LtKey::poison;
    reset_key();
    const int ktail = kgt < kearly ? 1 : 0;
    const int ktail_calls = LtKey::calls;
    const int ktail_poison = LtKey::poison;
    std::printf(
        "klt empty %d calls %d ctors %d poison %d short %d scalls %d sctors %d rev %d rcalls %d rctors %d "
        "prefix %d pcalls %d ppoison %d pctors %d longer %d lcalls %d lpoison %d lctors %d "
        "miss %d mcalls %d mpoison %d head %d hcalls %d hpoison %d tail %d tcalls %d tpoison %d\n",
        kempty_lt, kempty_calls, kempty_ctors, kempty_poison, kshort_lt, kshort_calls, kshort_ctors, krev_lt,
        krev_calls, krev_ctors, kprefix, kprefix_calls, kprefix_poison, kprefix_ctors, klonger, klonger_calls,
        klonger_poison, klonger_ctors, kmiss, kmiss_calls, kmiss_poison, khead, khead_calls, khead_poison, ktail,
        ktail_calls, ktail_poison);

    std::printf(
        "gt empty %d nonempty %d rev %d same %d mid %d lt %d longer %d prefix %d one %d self %d const %d\n",
        none > none ? 1 : 0, one > none ? 1 : 0, none > one ? 1 : 0, two > two ? 1 : 0, other > two ? 1 : 0,
        two > other ? 1 : 0, three > two ? 1 : 0, two > three ? 1 : 0, two > one ? 1 : 0, two > two ? 1 : 0,
        one > cone ? 1 : 0);

    reset_key();
    const int gempty = kempty > kempty2 ? 1 : 0;
    const int gempty_calls = LtKey::calls;
    const int gempty_ctors = LtKey::ctors;
    const int gempty_poison = LtKey::poison;
    reset_key();
    const int gnonempty = kone > kempty ? 1 : 0;
    const int gnonempty_calls = LtKey::calls;
    const int gnonempty_ctors = LtKey::ctors;
    reset_key();
    const int grev = kempty > kone ? 1 : 0;
    const int grev_calls = LtKey::calls;
    const int grev_ctors = LtKey::ctors;
    reset_key();
    const int gprefix = kb > ka ? 1 : 0;
    const int gprefix_calls = LtKey::calls;
    const int gprefix_poison = LtKey::poison;
    const int gprefix_ctors = LtKey::ctors;
    reset_key();
    const int glonger = ka > kb ? 1 : 0;
    const int glonger_calls = LtKey::calls;
    const int glonger_poison = LtKey::poison;
    const int glonger_ctors = LtKey::ctors;
    reset_key();
    const int gmiss = kmiss_set > kb ? 1 : 0;
    const int gmiss_calls = LtKey::calls;
    const int gmiss_poison = LtKey::poison;
    reset_key();
    const int ghead = kgt > kearly ? 1 : 0;
    const int ghead_calls = LtKey::calls;
    const int ghead_poison = LtKey::poison;
    reset_key();
    const int gtail = kearly > kgt ? 1 : 0;
    const int gtail_calls = LtKey::calls;
    const int gtail_poison = LtKey::poison;
    std::printf(
        "kgt empty %d calls %d ctors %d poison %d nonempty %d ncalls %d nctors %d rev %d rcalls %d rctors %d "
        "prefix %d pcalls %d ppoison %d pctors %d longer %d lcalls %d lpoison %d lctors %d "
        "miss %d mcalls %d mpoison %d head %d hcalls %d hpoison %d tail %d tcalls %d tpoison %d\n",
        gempty, gempty_calls, gempty_ctors, gempty_poison, gnonempty, gnonempty_calls, gnonempty_ctors, grev,
        grev_calls, grev_ctors, gprefix, gprefix_calls, gprefix_poison, gprefix_ctors, glonger, glonger_calls,
        glonger_poison, glonger_ctors, gmiss, gmiss_calls, gmiss_poison, ghead, ghead_calls, ghead_poison, gtail,
        gtail_calls, gtail_poison);

    std::flat_set<OrdKey> oempty;
    std::flat_set<OrdKey> oempty2;
    const int oa[] = {1};
    const int ob[] = {2};
    const int oc[] = {1, 2};
    const int od[] = {1, 9};
    std::flat_set<OrdKey> oone;
    std::flat_set<OrdKey> otwo;
    std::flat_set<OrdKey> oone2;
    std::flat_set<OrdKey> o12;
    std::flat_set<OrdKey> o19;
    add_ord(oone, oa, 1);
    add_ord(otwo, ob, 1);
    add_ord(oone2, oa, 1);
    add_ord(o12, oc, 2);
    add_ord(o19, od, 2);
    reset_ord();
    const int oempty_gt = oempty > oempty2 ? 1 : 0;
    const int oempty_lt = OrdKey::lt;
    const int oempty_kg = OrdKey::gt;
    reset_ord();
    const int ononempty_gt = oone > oempty ? 1 : 0;
    const int ononempty_lt = OrdKey::lt;
    const int ononempty_kg = OrdKey::gt;
    reset_ord();
    const int orev_gt = oempty > oone ? 1 : 0;
    const int orev_lt = OrdKey::lt;
    const int orev_kg = OrdKey::gt;
    reset_ord();
    const int ogt = otwo > oone ? 1 : 0;
    const int ogt_lt = OrdKey::lt;
    const int ogt_kg = OrdKey::gt;
    reset_ord();
    const int olt = oone > otwo ? 1 : 0;
    const int olt_lt = OrdKey::lt;
    const int olt_kg = OrdKey::gt;
    reset_ord();
    const int osame = oone > oone2 ? 1 : 0;
    const int osame_lt = OrdKey::lt;
    const int osame_kg = OrdKey::gt;
    reset_ord();
    const int omid = o19 > o12 ? 1 : 0;
    const int omid_lt = OrdKey::lt;
    const int omid_kg = OrdKey::gt;
    reset_ord();
    const int oearly = o12 > o19 ? 1 : 0;
    const int oearly_lt = OrdKey::lt;
    const int oearly_kg = OrdKey::gt;
    std::printf(
        "ord empty %d lt %d gt %d nonempty %d nlt %d ngt %d rev %d rlt %d rgt %d "
        "held %d hlt %d hgt %d less %d llt %d lgt %d same %d slt %d sgt %d "
        "mid %d mlt %d mgt %d early %d elt %d egt %d\n",
        oempty_gt, oempty_lt, oempty_kg, ononempty_gt, ononempty_lt, ononempty_kg, orev_gt, orev_lt, orev_kg, ogt,
        ogt_lt, ogt_kg, olt, olt_lt, olt_kg, osame, osame_lt, osame_kg, omid, omid_lt, omid_kg, oearly, oearly_lt,
        oearly_kg);
    return 0;
}
