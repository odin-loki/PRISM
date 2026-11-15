/* PRISM hard benchmark: protocol state machines driven by an input array.
 * *_ok: no undefined behaviour for any input satisfying the contract;
 * *_bad: a planted violation. See README.md. */

/* HDLC-like framer: 0x7e flag, length nibble, payload, trailer. */
// requires: \valid_read(in + (0..n-1))
int frame_states_ok(const unsigned char *in, int n) {
    if (n < 0 || n > 48) return -1;
    int st = 0, cnt = 0, len = 0, frames = 0;
    for (int i = 0; i < n; i++) {
        unsigned char b = in[i];
        switch (st) {
        case 0: if (b == 0x7e) st = 1; break;
        case 1: len = b & 15; cnt = 0; st = len ? 2 : 0; break;
        case 2: cnt++; if (cnt == len) st = 3; break;
        case 3: if (b == 0x7e) { frames++; st = 1; } else st = 0; break;
        default: st = 0; break;
        }
    }
    return frames * 4 + st;
}

/* Same framer, but the payload is copied into a 15-byte buffer with an
 * off-by-one: len 15 writes buf[15]. */
// requires: \valid_read(in + (0..n-1))
int frame_copy_bad(const unsigned char *in, int n) {
    unsigned char buf[15];
    if (n < 0 || n > 40) return -1;
    int st = 0, cnt = 0, len = 0, sum = 0;
    for (int i = 0; i < n; i++) {
        unsigned char b = in[i];
        switch (st) {
        case 0: if (b == 0x7e) st = 1; break;
        case 1: len = b & 15; cnt = 0; st = len ? 2 : 0; break;
        case 2:
            buf[cnt] = b;                          /* cnt <= len-1 <= 14: ok */
            cnt++;
            if (cnt == len) { st = 3; buf[len] = 0; } /* len == 15: buf[15] */
            break;
        default: sum += b; st = 0; break;
        }
    }
    return sum + st;
}

/* TCP-like connection state machine over an event array; the counters are
 * bounded by the trip count, so no overflow. */
// requires: \valid_read(ev + (0..n-1))
int tcp_events_ok(const int *ev, int n) {
    if (n < 0 || n > 32) return -1;
    int st = 0, sent = 0, acked = 0, resets = 0;
    for (int i = 0; i < n; i++) {
        int e = ev[i] & 7;
        switch (st) {
        case 0: if (e == 1) st = 1; break;                 /* CLOSED -> SYN_SENT */
        case 1: if (e == 2) st = 2; else if (e == 7) { st = 0; resets++; } break;
        case 2: if (e == 3) sent++; else if (e == 4) { if (acked < sent) acked++; }
                else if (e == 5) st = 3; else if (e == 7) { st = 0; resets++; } break;
        case 3: if (e == 6) st = 0; break;                 /* FIN_WAIT -> CLOSED */
        }
    }
    return (sent - acked) + resets * 100 + st;
}

/* The window counter grows by the event value with no bound: overflow. */
// requires: \valid_read(ev + (0..n-1))
int tcp_window_bad(const int *ev, int n) {
    if (n < 0 || n > 16) return -1;
    int st = 0, win = 0;
    for (int i = 0; i < n; i++) {
        int e = ev[i];
        switch (st) {
        case 0: if (e == 1) st = 1; break;
        case 1: win += e; if (e < 0) st = 0; break;        /* win + e overflows */
        }
    }
    return win;
}

/* Fixed-length (constant trip count) scan of a 64-entry command table
 * computed from x; no pointer parameters. */
int cmd_table_ok(unsigned x) {
    unsigned char tab[64];
    for (int i = 0; i < 64; i++) tab[i] = (unsigned char)((x >> (i & 31)) ^ (unsigned)i);
    int st = 0, hits = 0;
    for (int i = 0; i < 64; i++) {
        switch (st) {
        case 0: st = (tab[i] & 1) ? 1 : 0; break;
        case 1: st = (tab[i] & 2) ? 2 : 0; break;
        case 2: hits++; st = 0; break;
        }
    }
    return hits + st;
}

/* Lexer: digits accumulate a value; the cap is too high for the multiply
 * that follows it (planted overflow: 300000000 * 10). */
// requires: \valid_read(s + (0..n-1))
int lex_number_bad(const char *s, int n) {
    if (n < 0 || n > 24) return -1;
    int v = 0, st = 0;
    for (int i = 0; i < n; i++) {
        char ch = s[i];
        if (st == 0 && ch >= '0' && ch <= '9') st = 1;
        if (st == 1) {
            if (ch < '0' || ch > '9') { st = 2; continue; }
            v = v * 10 + (ch - '0');                        /* overflows */
            if (v > 300000000) v = 300000000;
        }
    }
    return v;
}

/* Same lexer with the cap checked before the multiply. */
// requires: \valid_read(s + (0..n-1))
int lex_number_ok(const char *s, int n) {
    if (n < 0 || n > 24) return -1;
    int v = 0, st = 0;
    for (int i = 0; i < n; i++) {
        char ch = s[i];
        if (st == 0 && ch >= '0' && ch <= '9') st = 1;
        if (st == 1) {
            if (ch < '0' || ch > '9') { st = 2; continue; }
            if (v > 100000) v = 100000;
            v = v * 10 + (ch - '0');
        }
    }
    return v;
}

/* Unbounded event loop (n is any int): the state stays in 0..3, which needs
 * an invariant (k-induction / Houdini). */
int unbounded_states_ok(int n, unsigned seed) {
    int st = 0;
    unsigned r = seed;
    while (n > 0) {
        r = r * 1103515245u + 12345u;
        switch (st) {
        case 0: st = (r >> 16) & 1 ? 1 : 0; break;
        case 1: st = (r >> 17) & 1 ? 2 : 3; break;
        case 2: st = 3; break;
        default: st = 0; break;
        }
        n--;
    }
    int tbl[4] = {1, 2, 3, 4};
    return tbl[st];
}
