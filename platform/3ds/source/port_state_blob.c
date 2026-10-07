/* Reserved space for the save state variable table.
 *
 * Zero in the object file; tools/gen_state_vars.py fills it in the linked ELF
 * (see port_state_vars.h). It is defined here and only read from
 * port_state_vars.c on purpose: a const array whose initialiser the compiler
 * can see would be folded to zeros at the read site. Do not add LTO to the
 * build without revisiting that. */
#include "port_state_vars.h"

__attribute__((used, aligned(4)))
const unsigned char gStateVarBlob[STATE_VAR_BLOB_SIZE] = { 0 };
