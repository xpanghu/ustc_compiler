/* The old textbook baseline calls this hook; the current Lab4 runtime does not.
 * Only linked into native baseline test executables, not into the compiler.
 * A runtime supplying the original symbol takes precedence over this shim.
 */
#include <stdio.h>
#include <stdlib.h>

__attribute__((weak)) void neg_idx_except(void) {
    fputs("negative index exception\n", stderr);
    exit(EXIT_FAILURE);
}
