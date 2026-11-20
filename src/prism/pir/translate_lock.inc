// Ghost lock and context state for the kernel specs (docs/KERNEL_CONCURRENCY.md,
// sections 2, 3, 5 and 12). Included by translate.cpp after translate_kernel.inc:
// these are Tr member functions. Reached from the intrinsics below, which the kernel
// specs (models/kernel/*.cpp) call, and from the contract markers that analysed code
// may call:
//   __prism_lock_acquire(p, spin)   __prism_lock_release(p, spin)   __prism_sleep_point()
//   __prism_sleep_if(may)           __prism_atomic_add(delta)       __prism_rcu_add(delta)
//   __prism_lock_entry(p, spin)     __prism_lock_exit(p)
//
// The state is one hidden object lk_ (like oom_: created in the prologue of the analysed
// function when a lock intrinsic is reachable from it, a MemKind::Static object, zero at
// entry, so the function starts with nothing held). Word layout:
//   words [0, K):   held locks (addresses; 0 = free slot)
//   words [K, 2K):  exit contract: locks the function declares it returns with held
//   word 2K:        spinlocks held (the atomic depth of spin_lock and irqsave)
//   word 2K+1:      preempt / bh count (preempt_count_add, __local_bh_disable_ip)
//   word 2K+2:      RCU read-side nesting (__rcu_read_lock)
//   word 2K+3:      overflow flag (a lock could not be recorded)
//
// Checks, each a property of its own class (prop "model", as __prism_check):
//   LOCK-DOUBLE          acquire of an address already held (NULL is not a lock)
//   LOCK-UNHELD-UNLOCK   release of an address not held, unless the set has overflowed
//   LOCK-HELD-ON-RETURN  at a return of the analysed function: a held lock that the exit
//                        contract does not declare
//   LOCK-EXIT-MISSING    a lock declared by __prism_lock_exit that is not held at the return
//   SLEEP-IN-ATOMIC      a sleeping primitive (may sleep) while spinlock or preempt depth > 0
//   PREEMPT-UNBALANCED   preempt / bh count below zero, or not restored at a return
//   RCU-UNBALANCED       RCU nesting below zero
//   RCU-HELD-ON-RETURN   a return inside an RCU read-side section
//   RCU-SLEEP            a sleeping primitive inside an RCU read-side section
// The updates are branch-free (selects), so no block is added and the CFG is unchanged.
//
// Contracts. __prism_lock_entry(p, spin) declares that the caller holds p on entry (spin:
// a spinlock, so the atomic depth starts positive). __prism_lock_exit(p) declares that p is
// still held when the function returns. Both are read only when the marker is in the
// analysed (root) function: an inlined callee's markers are no-ops, its lock operations act
// on the caller's state. Assumption linux-lock-contract names the contract as trusted by
// the callers (the call site is not checked here: that is the module pass, phase B).
//
// Scope: the atomic depth is spinlocks and preempt/bh counts; interrupt-disabled regions
// (local_irq_save, arch_local_irq_*) are inline asm in kernel headers and are not seen.
// Not checked here: lock order, hardirq and softirq contexts, LOCK-CTX-MISMATCH, RCU
// grace periods, per-CPU data.

namespace {
constexpr unsigned kLockSlots = 8;
constexpr unsigned kWSpin = 2 * kLockSlots;
constexpr unsigned kWPre = kWSpin + 1;
constexpr unsigned kWRcu = kWSpin + 2;
constexpr unsigned kWOver = kWSpin + 3;
constexpr unsigned kWords = kWSpin + 4;

Arg bool_and(Tr& t, int b, Arg x, Arg y) { return t.assign(b, Op::And, 1, {x, y}, "lk"); }
Arg bool_or(Tr& t, int b, Arg x, Arg y) { return t.assign(b, Op::Or, 1, {x, y}, "lk"); }
Arg bool_not(Tr& t, int b, Arg x) { return t.assign(b, Op::Xor, 1, {x, Arg::c(1, 1)}, "lk"); }
Arg nonzero(Tr& t, int b, Arg x) { return t.assign(b, Op::Ne, 1, {x, Arg::c(x.width, 0)}, "lk"); }
Arg widen(Tr& t, int b, Arg x, unsigned w) { return x.width == w ? x : t.assign(b, Op::SExt, w, {x}, "d"); }

bool is_lock_intrinsic(const std::string& n) {
    return n == "__prism_lock_acquire" || n == "__prism_lock_release" || n == "__prism_sleep_point" ||
           n == "__prism_lock_entry" || n == "__prism_lock_exit" || n == "__prism_atomic_add" ||
           n == "__prism_rcu_add";
}
}  // namespace

// The state object is allocated in the prologue of the analysed function (run_frame), when
// a lock intrinsic is reachable from it: it then exists on every path (no lazy allocation
// inside a branch). __prism_sleep_if is not in the list: without a lock intrinsic reachable
// the depth is always zero, so the check is trivially true and needs no state (kmalloc
// users are not given an extra object).
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
                if (is_lock_intrinsic(in.callee)) return true;
                if (const auto* h = m.find(in.callee)) todo.push_back(h);
            }
    }
    return false;
}

// Which lock intrinsics the call tree of f reaches: bit 0 acquire, bit 1 release (--pir-lock-wrappers).
int Tr::lock_effects(const ir::Module& m, const ir::Function& f) {
    std::set<const ir::Function*> seen;
    std::vector<const ir::Function*> todo{&f};
    int fx = 0;
    while (!todo.empty()) {
        const auto* g = todo.back();
        todo.pop_back();
        if (!seen.insert(g).second) continue;
        for (auto& bl : g->blocks)
            for (auto& in : bl.insts) {
                if ((in.op != "call" && in.op != "invoke") || in.callee.empty()) continue;
                if (in.callee == "__prism_lock_acquire") fx |= 1;
                if (in.callee == "__prism_lock_release") fx |= 2;
                if (const auto* h = m.find(in.callee)) todo.push_back(h);
            }
    }
    return fx;
}

void Tr::emit_lock_state() {
    const unsigned wb = ptr_w() / 8;
    lk_ = mt.alloc(-1, Arg::c(64, kWords * wb), MemKind::Static, 1, "lock state (PRISM ghost, kernel specs)", 0, lk_var_);
    lk_pending_ = false;
}

Arg Tr::lock_word(int b, unsigned i, int line) {
    const unsigned wb = ptr_w() / 8;
    if (!lk_) throw Unenc{"UNENCODED: ghost lock state used but not allocated"};
    return at(b, *lk_, static_cast<int64_t>(i * wb), line);
}

// i1: p is non-null and already in the slot array at word base
Arg Tr::lock_contains(int b, unsigned base, Arg p, int line) {
    const unsigned W = ptr_w();
    Arg found = Arg::c(1, 0);
    for (unsigned i = 0; i < kLockSlots; ++i) {
        Arg s = ld(b, lock_word(b, base + i, line), W, line);
        found = bool_or(*this, b, found, assign(b, Op::Eq, 1, {s, p}, "lk"));
    }
    return bool_and(*this, b, nonzero(*this, b, p), found);
}

// records p in the first free slot of the array at word base when fresh holds; a full array sets the overflow flag
void Tr::lock_insert(int b, unsigned base, Arg p, Arg fresh, int line) {
    const unsigned W = ptr_w();
    const Arg zero = Arg::c(W, 0);
    Arg placed = Arg::c(1, 0);
    for (unsigned i = 0; i < kLockSlots; ++i) {
        Arg s = ld(b, lock_word(b, base + i, line), W, line);
        Arg is_free = assign(b, Op::Eq, 1, {s, zero}, "lk");
        Arg take = bool_and(*this, b, fresh, bool_and(*this, b, is_free, bool_not(*this, b, placed)));
        st(b, lock_word(b, base + i, line), assign(b, Op::Select, W, {take, p, s}, "slot"), line);
        placed = bool_or(*this, b, placed, take);
    }
    Arg full = bool_and(*this, b, fresh, bool_not(*this, b, placed));
    Arg ov = ld(b, lock_word(b, kWOver, line), W, line);
    st(b, lock_word(b, kWOver, line), assign(b, Op::Select, W, {full, Arg::c(W, 1), ov}, "ov"), line);
}

// word w += delta (signed; a spinlock depth is +1 / -1, selected by the caller)
void Tr::lock_bump(int b, unsigned w, Arg delta, int line) {
    const unsigned W = ptr_w();
    Arg d = widen(*this, b, delta, W);
    Arg cur_v = ld(b, lock_word(b, w, line), W, line);
    st(b, lock_word(b, w, line), assign(b, Op::Add, W, {cur_v, d}, "depth"), line);
}

// A held lock at a return of the analysed function: each held lock must be declared by an
// exit contract (__prism_lock_exit); each declared exit lock must still be held.
void Tr::lock_return_check(int b, int line) {
    if (!lk_) return;
    const unsigned W = ptr_w();
    std::vector<Arg> held, exits;
    for (unsigned i = 0; i < kLockSlots; ++i) held.push_back(ld(b, lock_word(b, i, line), W, line));
    for (unsigned i = 0; i < kLockSlots; ++i) exits.push_back(ld(b, lock_word(b, kLockSlots + i, line), W, line));
    Arg undeclared = Arg::c(1, 0);
    for (auto& h : held) {
        Arg declared = Arg::c(1, 0);
        for (auto& x : exits) declared = bool_or(*this, b, declared, assign(b, Op::Eq, 1, {h, x}, "lk"));
        undeclared = bool_or(*this, b, undeclared, bool_and(*this, b, nonzero(*this, b, h), bool_not(*this, b, declared)));
    }
    if (lock_wrapper_ == 1) {
        // --pir-lock-wrappers: an acquire-only wrapper (rtnl_lock, ssb_buses_lock): returning with the lock held is
        // what it is for; the pairing is its callers' property
        note_assumption("lock-wrapper: @" + root_name_ + " acquires locks and releases none on any path: it returns with "
                        "them held by design (its callers release them; LOCK-HELD-ON-RETURN is not checked here)");
    } else {
        check(b, undeclared, "model", "LOCK-HELD-ON-RETURN",
              "function returns with a lock still held (taken here, not released, not declared by __prism_lock_exit)", line);
    }
    Arg missing = Arg::c(1, 0);
    for (auto& x : exits) {
        Arg is_held = Arg::c(1, 0);
        for (auto& h : held) is_held = bool_or(*this, b, is_held, assign(b, Op::Eq, 1, {h, x}, "lk"));
        missing = bool_or(*this, b, missing, bool_and(*this, b, nonzero(*this, b, x), bool_not(*this, b, is_held)));
    }
    check(b, missing, "model", "LOCK-EXIT-MISSING",
          "function is declared (__prism_lock_exit) to return with a lock held, but the lock is released or not taken", line);
    Arg pre = ld(b, lock_word(b, kWPre, line), W, line);
    check(b, nonzero(*this, b, pre), "model", "PREEMPT-UNBALANCED",
          "function returns with a preempt or bh count not restored", line);
    Arg rcu = ld(b, lock_word(b, kWRcu, line), W, line);
    check(b, nonzero(*this, b, rcu), "model", "RCU-HELD-ON-RETURN",
          "function returns inside an RCU read-side section (rcu_read_lock without rcu_read_unlock)", line);
}

void Tr::lock_take(int b, Arg p, Arg spin, int line) {
    note_assumption("linux-lock-overflow: more than 8 locks held at once are not tracked (the held set overflows)");
    const unsigned W = ptr_w();
    Arg nonnull = nonzero(*this, b, p);
    Arg dup = lock_contains(b, 0, p, line);
    check(b, dup, "model", "LOCK-DOUBLE", "lock taken while already held by this function (self-deadlock)", line);
    lock_insert(b, 0, p, bool_and(*this, b, nonnull, bool_not(*this, b, dup)), line);
    Arg nz = nonzero(*this, b, spin);
    lock_bump(b, kWSpin, assign(b, Op::Select, W, {nz, Arg::c(W, 1), Arg::c(W, 0)}, "depth"), line);
}

void Tr::lock_drop(int b, Arg p, Arg spin, int line) {
    const unsigned W = ptr_w();
    const Arg zero = Arg::c(W, 0);
    Arg nonnull = nonzero(*this, b, p);
    std::vector<Arg> hit;
    Arg found = Arg::c(1, 0);
    for (unsigned i = 0; i < kLockSlots; ++i) {
        Arg s = ld(b, lock_word(b, i, line), W, line);
        Arg h = bool_and(*this, b, nonnull, assign(b, Op::Eq, 1, {s, p}, "lk"));
        hit.push_back(h);
        found = bool_or(*this, b, found, h);
    }
    Arg ov = ld(b, lock_word(b, kWOver, line), W, line);
    Arg overflowed = assign(b, Op::Ne, 1, {ov, zero}, "lk");
    if (lock_wrapper_ == 2) {
        // --pir-lock-wrappers: a release-only wrapper (rtnl_unlock, ssb_buses_unlock) releases a lock its caller holds
        note_assumption("lock-wrapper: @" + root_name_ + " releases locks and acquires none on any path: the caller holds "
                        "them on entry (LOCK-UNHELD-UNLOCK is not checked here)");
    } else {
        check(b, bool_and(*this, b, bool_not(*this, b, found), bool_not(*this, b, overflowed)), "model",
              "LOCK-UNHELD-UNLOCK", "lock released that this function does not hold", line);
    }
    for (unsigned i = 0; i < kLockSlots; ++i) {
        Arg s = ld(b, lock_word(b, i, line), W, line);
        st(b, lock_word(b, i, line), assign(b, Op::Select, W, {hit[i], zero, s}, "slot"), line);
    }
    Arg nz = nonzero(*this, b, spin);
    Arg dec = assign(b, Op::Select, W, {nz, Arg::c(W, 1), Arg::c(W, 0)}, "depth");
    lock_bump(b, kWSpin, assign(b, Op::Sub, W, {Arg::c(W, 0), dec}, "depth"), line);
}

// __prism_lock_entry / __prism_lock_exit, root frame only (see the header comment)
void Tr::lock_declare(int b, bool entry, Arg p, Arg spin, int line) {
    if (!lk_) return;
    note_assumption("linux-lock-contract: the entry and exit locks declared by __prism_lock_entry / __prism_lock_exit "
                    "are the caller's contract (the call sites are not checked)");
    const unsigned W = ptr_w();
    Arg nonnull = nonzero(*this, b, p);
    if (entry) {
        Arg dup = lock_contains(b, 0, p, line);
        lock_insert(b, 0, p, bool_and(*this, b, nonnull, bool_not(*this, b, dup)), line);
        Arg nz = nonzero(*this, b, spin);
        lock_bump(b, kWSpin, assign(b, Op::Select, W, {nz, Arg::c(W, 1), Arg::c(W, 0)}, "depth"), line);
    } else {
        Arg dup = lock_contains(b, kLockSlots, p, line);
        lock_insert(b, kLockSlots, p, bool_and(*this, b, nonnull, bool_not(*this, b, dup)), line);
    }
}

// preempt_count_add / __local_bh_disable_ip (+delta) and their releases (-delta): any frame,
// since the count belongs to the analysed call path
void Tr::atomic_add(int b, Arg delta, int line) {
    if (!lk_) return;
    const unsigned W = ptr_w();
    Arg d = widen(*this, b, delta, W);
    Arg pre = ld(b, lock_word(b, kWPre, line), W, line);
    Arg nw = assign(b, Op::Add, W, {pre, d}, "pre");
    check(b, assign(b, Op::Slt, 1, {nw, Arg::c(W, 0)}, "lk"), "model", "PREEMPT-UNBALANCED",
          "preempt or bh count released more than it was taken", line);
    st(b, lock_word(b, kWPre, line), nw, line);
}

// rcu_read_lock (+1) and rcu_read_unlock (-1)
void Tr::rcu_add(int b, Arg delta, int line) {
    if (!lk_) return;
    const unsigned W = ptr_w();
    Arg d = widen(*this, b, delta, W);
    Arg cur_v = ld(b, lock_word(b, kWRcu, line), W, line);
    Arg nw = assign(b, Op::Add, W, {cur_v, d}, "rcu");
    check(b, assign(b, Op::Slt, 1, {nw, Arg::c(W, 0)}, "lk"), "model", "RCU-UNBALANCED",
          "rcu_read_unlock without a matching rcu_read_lock", line);
    st(b, lock_word(b, kWRcu, line), nw, line);
}

// a sleeping primitive: may is the call's sleep condition (1 bit, or an int flag)
void Tr::sleep_if(int b, Arg may, int line) {
    if (!lk_) return;  // no lock intrinsic reachable: the depth is zero (see reaches_lock_state)
    const unsigned W = ptr_w();
    Arg may1 = may.width == 1 ? may : nonzero(*this, b, may);
    Arg spin = ld(b, lock_word(b, kWSpin, line), W, line);
    Arg pre = ld(b, lock_word(b, kWPre, line), W, line);
    Arg total = assign(b, Op::Add, W, {spin, pre}, "depth");
    Arg atomic = assign(b, Op::Sgt, 1, {total, Arg::c(W, 0)}, "lk");
    check(b, bool_and(*this, b, may1, atomic), "model", "SLEEP-IN-ATOMIC",
          "sleeping primitive (may sleep) while a spinlock is held or preemption is disabled", line);
    Arg rcu = ld(b, lock_word(b, kWRcu, line), W, line);
    Arg in_rcu = assign(b, Op::Sgt, 1, {rcu, Arg::c(W, 0)}, "lk");
    check(b, bool_and(*this, b, may1, in_rcu), "model", "RCU-SLEEP",
          "sleeping primitive inside an RCU read-side section", line);
}

void Tr::sleep_point(int b, int line) { sleep_if(b, Arg::c(1, 1), line); }

// --lock-contracts table (TSV): contracts for unmodified code, no markers in the source
bool Tr::lock_contracted(const std::string& fn) const {
    for (auto& c : opt.lock_contracts)
        if (c.function == fn) return true;
    return false;
}

// root frame only, after lk_ exists: declares the table's entry and exit locks in the entry block
void Tr::lock_table(Frame& fr) {
    bool any = false;
    for (auto& c : opt.lock_contracts) {
        if (c.function != fr.f->name) continue;
        // field:TYPE.N rows feed the lock-order pass only (lock_order.cpp); the ghost lock state is per address
        if (c.lock.starts_with("field:")) continue;
        Arg p;
        if (c.lock.starts_with("global:")) {
            p = mt.global(c.lock.substr(7));
        } else {
            const auto n = static_cast<std::size_t>(std::stoul(c.lock.substr(4)));
            if (n >= fr.f->params.size() || !fr.env.count(fr.f->params[n].name))
                throw Unenc{"UNENCODED: lock contract argument " + c.lock + " of " + fr.f->name};
            p = fr.env.at(fr.f->params[n].name);
        }
        const int b = head.at(fr.prefix + fr.f->blocks.front().name);
        const Arg spin = Arg::c(1, c.spin ? 1 : 0);
        if (c.action == "releases" || c.action == "requires-held") lock_declare(b, true, p, spin, 0);
        if (c.action == "acquires" || c.action == "requires-held") lock_declare(b, false, p, spin, 0);
        any = true;
    }
    if (any)
        note_assumption("lock-contract-table: the lock contract of --lock-contracts (TSV) for " + fr.f->name +
                        " is trusted (its call sites are not checked)");
}
