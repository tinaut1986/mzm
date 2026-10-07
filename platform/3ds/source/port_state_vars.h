#ifndef PORT_STATE_VARS_H
#define PORT_STATE_VARS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The decomp's mutable globals (the bracket between __ss_data_start/_end and
 * __ss_bss_start/_end, see 3dsx.ld) by name, so a save state can be restored
 * into a build whose globals have moved, grown or gone.
 *
 * The table is not C: it is built from the linked ELF by
 * tools/gen_state_vars.py and patched into the array reserved by port_state_blob.c. A binary that was not post-processed has an empty table
 * (PortStateVars_Ready() is false) and save states fall back to the
 * build-bound format. Names: symbol name without the compiler's `.<digits>`
 * suffix, `#<n>` appended on a repeat. */

#define STATE_VAR_BLOB_SIZE 0x8000

typedef struct {
    const char* name;
    void*       ptr;
    uint32_t    size;
} PortStateVar;

bool PortStateVars_Ready(void);
int  PortStateVars_Count(void);
const PortStateVar* PortStateVars_At(int index);
const PortStateVar* PortStateVars_Find(const char* name);
/* The variable holding address `p`, and p's offset in it; NULL if none. */
const PortStateVar* PortStateVars_Containing(uintptr_t p, uint32_t* outOffset);

#ifdef __cplusplus
}
#endif

#endif /* PORT_STATE_VARS_H */
