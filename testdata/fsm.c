enum { ST_IDLE = 0, ST_WORK = 1, ST_BAD = 2 };

int fsm_step(int state, int ev) {
    switch (state) {
    case ST_IDLE:
        if (ev) state = ST_WORK;
        break;
    case ST_WORK:
        if (ev) state = ST_IDLE;
        else state = ST_BAD;
        break;
    default:
        break;
    }
    return state;
}
