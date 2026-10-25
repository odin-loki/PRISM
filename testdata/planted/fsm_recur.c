/* Two-state cycle: GF LIVE_IDLE has a safety approximation G(F_k LIVE_IDLE). */
enum { LIVE_IDLE = 0, LIVE_ACK = 1, LIVE_BUSY = 2 };

int fsm_recur(int state, int ev) {
    switch (state) {
    case LIVE_IDLE:
        state = LIVE_ACK;
        break;
    case LIVE_ACK:
        state = LIVE_IDLE;
        break;
    }
    return state;
}

/* Settles in LIVE_IDLE: FG LIVE_IDLE has a safety approximation F_k (G LIVE_IDLE). */
int fsm_settle(int state, int ev) {
    switch (state) {
    case LIVE_BUSY:
        state = LIVE_IDLE;
        break;
    case LIVE_IDLE:
        break;
    }
    return state;
}

/* Fall-through labels: REQ and RETRY share the ACK dest. */
enum { FT_REQ = 0, FT_RETRY = 1, FT_ACK = 2 };

int fsm_fall(int state, int ev) {
    switch (state) {
    case FT_REQ:
    case FT_RETRY:
        state = FT_ACK;
        break;
    case FT_ACK:
        state = FT_REQ;
        break;
    }
    return state;
}

/* Parenthesized dests must still be transitions, not self-loops. */
int fsm_paren(int state, int ev) {
    switch (state) {
    case LIVE_IDLE:
        state = (LIVE_ACK);
        break;
    case LIVE_ACK:
        state = (LIVE_IDLE);
        break;
    }
    return state;
}

/* One named case plus default: still a switch(state) machine. */
int fsm_default(int state, int ev) {
    switch (state) {
    case LIVE_IDLE:
        state = LIVE_ACK;
        break;
    default:
        state = LIVE_IDLE;
        break;
    }
    return state;
}

/* switch(ev) is not the plant, even though the body assigns state. */
int fsm_ev_only(int state, int ev) {
    switch (ev) {
    case 0:
        state = LIVE_ACK;
        break;
    case 1:
        state = LIVE_IDLE;
        break;
    }
    return state;
}
