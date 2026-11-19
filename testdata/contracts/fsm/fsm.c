/* State-machine step functions: a table-free switch on (state, event) and a
 * transition table, both total over their state space. Scalar: the
 * contracts stage proves them directly. */
/* --- contract-library prelude (testdata/contracts/README.md) --- */
#include <assert.h>
/* --- end prelude --- */

enum { ST_IDLE, ST_OPEN, ST_DATA, ST_CLOSED, ST_NUM };
enum { EV_OPEN, EV_BYTE, EV_END, EV_RESET, EV_NUM };

/*@ requires 0 <= state < ST_NUM;
    assigns \nothing;
    ensures 0 <= \result < ST_NUM;
    ensures ev == EV_RESET ==> \result == ST_IDLE;
    ensures state == ST_CLOSED && ev != EV_RESET ==> \result == ST_CLOSED;
    ensures \result == ST_DATA ==> state == ST_OPEN || state == ST_DATA;
*/
int fsm_step(int state, int ev) {
    if (ev == EV_RESET) return ST_IDLE;
    switch (state) {
    case ST_IDLE:
        return ev == EV_OPEN ? ST_OPEN : ST_IDLE;
    case ST_OPEN:
        return ev == EV_BYTE ? ST_DATA : ev == EV_END ? ST_CLOSED : ST_OPEN;
    case ST_DATA:
        return ev == EV_END ? ST_CLOSED : ST_DATA;
    default:
        return ST_CLOSED;
    }
}

static const unsigned char fsm_table[ST_NUM][EV_NUM] = {
    /* OPEN      BYTE     END        RESET */
    {ST_OPEN, ST_IDLE, ST_IDLE, ST_IDLE},       /* IDLE */
    {ST_OPEN, ST_DATA, ST_CLOSED, ST_IDLE},     /* OPEN */
    {ST_DATA, ST_DATA, ST_CLOSED, ST_IDLE},     /* DATA */
    {ST_CLOSED, ST_CLOSED, ST_CLOSED, ST_IDLE}, /* CLOSED */
};

/* Out-of-range events are ignored (the state is kept); an out-of-range
 * state (corrupted) goes back to IDLE. Total: safe for every input. */
/*@ assigns \nothing;
    ensures 0 <= \result < ST_NUM;
    ensures 0 <= state < ST_NUM && (ev < 0 || ev >= EV_NUM) ==> \result == state;
    ensures (state < 0 || state >= ST_NUM) ==> \result == ST_IDLE;
*/
int fsm_table_step(int state, int ev) {
    if (state < 0 || state >= ST_NUM) return ST_IDLE;
    if (ev < 0 || ev >= EV_NUM) return state;
    return fsm_table[state][ev];
}

/* The table and the switch are the same machine. */
/*@ requires 0 <= state < ST_NUM && 0 <= ev < EV_NUM;
    ensures \result == 1;
*/
int fsm_equiv(int state, int ev) { return fsm_step(state, ev) == fsm_table_step(state, ev); }

/* Planted twin of a contract: "DATA is only reached from OPEN". */
/*@ requires 0 <= state < ST_NUM;
    ensures \result == ST_DATA ==> state == ST_OPEN;
*/
int fsm_step_bad(int state, int ev) { return fsm_step(state, ev); }

/* Planted twin: the table indexed with an unchecked state. */
int fsm_table_oob_bad(int state, int ev) {
    if (ev < 0 || ev >= EV_NUM || state < 0 || state > ST_NUM) return 0;
    return fsm_table[state][ev];
}
