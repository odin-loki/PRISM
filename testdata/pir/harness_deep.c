/* --pir-harness-deep (docs/HARNESS_PHASE2.md "Deeper pointer fields"): depth 4 instead of 2. The bound that was
 * applied, the deepest level reached and what was cut are named in the assumptions and in the --jsonl record. */

struct node { int v; struct node *next; };

int list_second(struct node *n) { return n->next->next->v; }                    /* depth 2: the default reaches it */
int list_fourth(struct node *n) { return n->next->next->next->next->v; }        /* depth 4: --pir-harness-deep */
int list_fifth(struct node *n) { return n->next->next->next->next->next->v; }   /* depth 5: past the deep bound */
