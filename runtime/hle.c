/* Host replacements for individual guest functions (see recomp/hooks.txt). */
#include "rt.h"
#include "funcs.h"

/* EA stream status: report the IOP-side stream as finished until the sound
 * driver itself runs; otherwise file loading waits for it forever. */
void hle_stream_done(EE *c) {
    R(2) = 1;
}
