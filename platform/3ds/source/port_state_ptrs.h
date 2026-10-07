#ifndef PORT_STATE_PTRS_H
#define PORT_STATE_PTRS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* What a save state does with the host pointers in the decomp's globals.
 * Read docs/3ds-save-states.md before changing the decomp's globals. */

/* A host pointer written down in terms that survive a rebuild and a restart:
 *   SPACE  into emulated GBA memory or the ROM image: which space + offset
 *   FUNC   a function: its name (the table in port_state_ptrs.c)
 *   VAR    into another decomp global: its name + offset */
typedef enum { PSR_SPACE = 1, PSR_FUNC = 2, PSR_VAR = 3 } PortStateRelocKind;

typedef struct {
    uint8_t  kind;
    char     holder[64];     /* the global that holds the pointer */
    uint32_t wordOff;        /* byte offset of the pointer in it */
    uint32_t code;           /* SPACE: ((space + 1) << 24) | offset in the space */
    char     target[64];     /* FUNC / VAR: the name */
    uint32_t targetOff;      /* VAR: offset in the target */
} PortStateReloc;

/* True for globals a save state leaves alone: the audio engine and the
 * port's own threads and buffers. They keep their live values. */
bool PortStatePtrs_IsExcluded(const char* var);

/* Finds the pointers in the globals a state saves and returns them as
 * relocations (malloc'd, caller frees; *out is NULL when there are none).
 * Debug builds also report (to the debug log) the globals that hold what
 * looks like a pointer but that no rule covers. */
int PortStatePtrs_Collect(PortStateReloc** out);

/* Writes the pointer back. A relocation whose target no longer exists leaves
 * a null pointer rather than a stale one. Returns false when it had to. */
bool PortStatePtrs_Apply(const PortStateReloc* r);

#ifdef __cplusplus
}
#endif

#endif /* PORT_STATE_PTRS_H */
