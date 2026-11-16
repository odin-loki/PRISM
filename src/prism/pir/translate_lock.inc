// Ghost lock state for the kernel lock specs (docs/KERNEL_CONCURRENCY.md,
// section 2.1 and 11; phase A). Included by translate.cpp after translate_kernel.inc:
// these are Tr member functions. Reached only from the intrinsics
//   __prism_lock_acquire(p, spin)   __prism_lock_release(p, spin)   __prism_sleep_point()
// which the kernel lock specs call (models/kernel/linux_lock.cpp).
//
// The state is one hidden object lk_ (like oom_: created on first use, a MemKind::Static
// object of kLockSlots + 2 words, zero at entry, so the analysed function starts with no
// lock held). The memory model keeps its contents across the calls of one analysis.
//   word i (i < kLockSlots): the address of a held lock, or 0 for a free slot;
//   word kLockSlots:         the atomic depth (spinlocks held, as preempt_count);
//   word kLockSlots + 1:     the overflow flag (set when a lock could not be recorded).
//
// Checks, each a property of its own class (prop "model", as __prism_check):
//   LOCK-DOUBLE         acquire of an address already held (NULL is not a lock: its
//                       read is checked by the spec);
//   LOCK-UNHELD-UNLOCK  release of an address not held, unless the set has overflowed;
//   LOCK-HELD-ON-RETURN a lock held at a ret
//   CTX-SLEEP-ATOMIC    a sleeping primitive while the atomic depth is positive.
// The updates are branch-free: each slot is rewritten with a select, so no block is
// added and the translated CFG of the analysed function is unchanged.
//
// LOCK-HELD-ON-RETURN: any lock still held at a return of the analysed function (entry set empty).
// Not checked here: entry contracts, lock order,
// interrupt and softirq context, RCU. A helper that releases a lock its caller holds
// is reported as LOCK-UNHELD-UNLOCK: a missing module contract, not a bug.

namespace {
constexpr unsigned kLockSlots = 8;
}  // namespace

namespace {
Arg bool_and(Tr& t, int b, Arg x, Arg y) { return t.assign(b, Op::And, 1, {x, y}, "lk"); }
Arg bool_or(Tr& t, int b, Arg x, Arg y) { return t.assign(b, Op::Or, 1, {x, y}, "lk"); }
Arg bool_not(Tr& t, int b, Arg x) { return t.assign(b, Op::Xor, 1, {x, Arg::c(1, 1)}, "lk"); }
}  // namespace

// The state object is allocated in the prologue of the analysed function (run_frame), when
// the call graph reaches a ghost intrinsic: it then exists on every path (no lazy allocation
// inside a branch).
bool Tr::reaches_lock_state(const ir::Module& m, const ir::Function& f) {
    std::set<const ir::Function*> seen;
    std::vector<const ir::Function*> todo{&f};
    while (!todo.empty()) {
        const auto* g = todo.back();
        todo.pop_back();
        if (!seen.insert(g).second) continue;
        for (auto& bl : g->blocks)
            for (auto& in : bl.insts) {
                if ((in.op != "call" && in.op != "invoke") || in.callee.empty()) continue;
                if (in.callee == "__prism_lock_acquire" || in.callee == "__prism_lock_release" ||
                    in.callee == "__prism_sleep_point")
                    return true;
                if (const auto* h = m.find(in.callee)) todo.push_back(h);
            }
    }
    return false;
}

void Tr::emit_lock_state() {
    const unsigned wb = ptr_w() / 8;
    lk_ = mt.alloc(-1, Arg::c(64, (kLockSlots + 2) * wb), MemKind::Static, 1, "lock state (PRISM ghost, kernel specs)", 0, lk_var_);
    lk_pending_ = false;
}

// A held lock at a return of the analysed function: the entry set is empty (no contract
// declares a lock held on entry yet), so any held lock is LOCK-HELD-ON-RETURN.
void Tr::lock_return_check(int b, int line) {
    if (!lk_) return;
    const unsigned W = ptr_w();
    Arg any = Arg::c(1, 0);
    for (unsigned i = 0; i < kLockSlots; ++i)
        any = bool_or(*this, b, any, assign(b, Op::Ne, 1, {ld(b, lock_word(b, i, line), W, line), Arg::c(W, 0)}, "lk"));
    check(b, any, "model", "LOCK-HELD-ON-RETURN", "function returns with a lock still held (taken here, not released)", line);
}

Arg Tr::lock_word(int b, unsigned i, int line) {
    const unsigned wb = ptr_w() / 8;
    if (!lk_) throw Unenc{"UNENCODED: ghost lock state used but not allocated"};
    return at(b, *lk_, static_cast<int64_t>(i * wb), line);
}

void Tr::lock_take(int b, Arg p, Arg spin, int line) {
    note_assumption("linux-lock-overflow: more than 8 locks held at once are not tracked (the held set overflows)");
    const unsigned W = ptr_w();
    const Arg zero = Arg::c(W, 0);
    Arg nonnull = assign(b, Op::Ne, 1, {p, zero}, "lk");
    std::vector<Arg> held;
    for (unsigned i = 0; i < kLockSlots; ++i) held.push_back(ld(b, lock_word(b, i, line), W, line));
    Arg dup = Arg::c(1, 0);
    for (auto& s : held) dup = bool_or(*this, b, dup, bool_and(*this, b, nonnull, assign(b, Op::Eq, 1, {s, p}, "lk")));
    check(b, bool_and(*this, b, nonnull, dup), "model", "LOCK-DOUBLE",
          "lock taken while already held by this function (self-deadlock)", line);
    Arg placed = Arg::c(1, 0);
    const Arg fresh = bool_and(*this, b, nonnull, bool_not(*this, b, dup));
    for (unsigned i = 0; i < kLockSlots; ++i) {
        Arg is_free = assign(b, Op::Eq, 1, {held[i], zero}, "lk");
        Arg take = bool_and(*this, b, fresh, bool_and(*this, b, is_free, bool_not(*this, b, placed)));
        Arg nv = assign(b, Op::Select, W, {take, p, held[i]}, "slot");
        st(b, lock_word(b, i, line), nv, line);
        placed = bool_or(*this, b, placed, take);
    }
    // a fresh lock with no free slot: record the overflow
    Arg full = bool_and(*this, b, fresh, bool_not(*this, b, placed));
    Arg ov = ld(b, lock_word(b, kLockSlots + 1, line), W, line);
    Arg ov_new = assign(b, Op::Select, W, {full, Arg::c(W, 1), ov}, "ov");
    st(b, lock_word(b, kLockSlots + 1, line), ov_new, line);
    {
        Arg nz = assign(b, Op::Ne, 1, {spin, Arg::c(spin.width, 0)}, "lk");
        Arg inc = assign(b, Op::Select, W, {nz, Arg::c(W, 1), Arg::c(W, 0)}, "depth");
        Arg depth = ld(b, lock_word(b, kLockSlots, line), W, line);
        st(b, lock_word(b, kLockSlots, line), assign(b, Op::Add, W, {depth, inc}, "depth"), line);
    }
}

void Tr::lock_drop(int b, Arg p, Arg spin, int line) {
    const unsigned W = ptr_w();
    const Arg zero = Arg::c(W, 0);
    Arg nonnull = assign(b, Op::Ne, 1, {p, zero}, "lk");
    std::vector<Arg> held;
    for (unsigned i = 0; i < kLockSlots; ++i) held.push_back(ld(b, lock_word(b, i, line), W, line));
    std::vector<Arg> hit;
    Arg found = Arg::c(1, 0);
    for (auto& s : held) {
        Arg h = bool_and(*this, b, nonnull, assign(b, Op::Eq, 1, {s, p}, "lk"));
        hit.push_back(h);
        found = bool_or(*this, b, found, h);
    }
    Arg ov = ld(b, lock_word(b, kLockSlots + 1, line), W, line);
    Arg overflowed = assign(b, Op::Ne, 1, {ov, zero}, "lk");
    check(b, bool_and(*this, b, bool_not(*this, b, found), bool_not(*this, b, overflowed)), "model",
          "LOCK-UNHELD-UNLOCK", "lock released that this function does not hold", line);
    for (unsigned i = 0; i < kLockSlots; ++i) {
        Arg nv = assign(b, Op::Select, W, {hit[i], zero, held[i]}, "slot");
        st(b, lock_word(b, i, line), nv, line);
    }
    {
        Arg nz = assign(b, Op::Ne, 1, {spin, Arg::c(spin.width, 0)}, "lk");
        Arg dec = assign(b, Op::Select, W, {nz, Arg::c(W, 1), Arg::c(W, 0)}, "depth");
        Arg depth = ld(b, lock_word(b, kLockSlots, line), W, line);
        st(b, lock_word(b, kLockSlots, line), assign(b, Op::Sub, W, {depth, dec}, "depth"), line);
    }
}

void Tr::sleep_point(int b, int line) {
    const unsigned W = ptr_w();
    Arg depth = ld(b, lock_word(b, kLockSlots, line), W, line);
    check(b, assign(b, Op::Sgt, 1, {depth, Arg::c(W, 0)}, "lk"), "model", "CTX-SLEEP-ATOMIC",
          "sleeping primitive (may sleep) while a spinlock is held", line);
}
