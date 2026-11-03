// Stacked case labels that name a scoped enumerator (`case Op::Add:`) run into
// the next arm with no statement between them: not a fallthrough. Reduced from
// src/prism/pir/encode.cpp Encoding::note_prov and astlint_checks.cpp
// api_assign_checks; the `::` inside a label used to end it early.
enum class Op { Select, Add, Sub, Mul, Trunc, Other };
enum class Use { Ptr, Fd, FdOrVoidCast };

int classify(Op op, int n) {
    int r = 0;
    switch (op) {
        case Op::Select:
            if (n == 3) r = 1;
            break;
        case Op::Add:
        case Op::Sub:
        case Op::Mul:
        case Op::Trunc: r = 2; break;
        default: break;
    }
    return r;
}

bool hits(Use u, int what) {
    bool hit = false;
    switch (u) {
        case Use::Ptr: hit = what == 0; break;
        case Use::Fd:
        case Use::FdOrVoidCast:
            hit = what == 1 || (u == Use::FdOrVoidCast && what == 4);
            break;
    }
    return hit;
}
