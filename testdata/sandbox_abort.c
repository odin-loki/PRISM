/* Planted crash for the OpenCodeInterpreter sandbox.
 * A successful compile+run of abort() is CRASH/FAILED, never PROVED.
 * Param keeps sanitize from auto-invoking this as a zero-arg TU.
 */
#include <stdlib.h>
int sandbox_abort(int x)
{
    (void)x;
    abort();
    return 0;
}
