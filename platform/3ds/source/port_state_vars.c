#include "port_state_vars.h"

#include <stdlib.h>
#include <string.h>

extern const unsigned char gStateVarBlob[STATE_VAR_BLOB_SIZE];

/* Bounds of the decomp save state bracket (3dsx.ld). */
extern char __ss_data_start[], __ss_data_end[];
extern char __ss_bss_start[], __ss_bss_end[];

#define SVT_MAGIC 0x31545653u   /* 'SVT1' */

static PortStateVar* sVars = NULL;     /* sorted by address */
static int sCount = 0;
static bool sParsed = false;

static int VarCmp(const void* a, const void* b) {
    const uintptr_t x = (uintptr_t)((const PortStateVar*)a)->ptr;
    const uintptr_t y = (uintptr_t)((const PortStateVar*)b)->ptr;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void Parse(void) {
    if (sParsed) return;
    sParsed = true;

    uint32_t head[4];
    memcpy(head, gStateVarBlob, sizeof(head));
    const uint32_t count = head[1], nameBytes = head[2];
    if (head[0] != SVT_MAGIC) return;
    const uint32_t tableBytes = count * 12u;
    if (16u + tableBytes + nameBytes > STATE_VAR_BLOB_SIZE) return;

    const unsigned char* table = gStateVarBlob + 16;
    const char* names = (const char*)(table + tableBytes);
    sVars = (PortStateVar*)malloc((size_t)count * sizeof(PortStateVar));
    if (!sVars) return;

    const uintptr_t dataSize = (uintptr_t)(__ss_data_end - __ss_data_start);
    const uintptr_t bssSize = (uintptr_t)(__ss_bss_end - __ss_bss_start);
    int n = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t nameOff, offReg, size;
        memcpy(&nameOff, table + i * 12u, 4);
        memcpy(&offReg, table + i * 12u + 4, 4);
        memcpy(&size, table + i * 12u + 8, 4);
        const bool bss = (offReg >> 31) != 0;
        const uint32_t off = offReg & 0x7FFFFFFFu;
        if (nameOff >= nameBytes) continue;
        if (off + size > (bss ? bssSize : dataSize)) continue;   /* table of another binary */
        sVars[n].name = names + nameOff;
        sVars[n].ptr = (bss ? __ss_bss_start : __ss_data_start) + off;
        sVars[n].size = size;
        ++n;
    }
    qsort(sVars, (size_t)n, sizeof(PortStateVar), VarCmp);
    sCount = n;
}

bool PortStateVars_Ready(void) { Parse(); return sCount > 0; }
int PortStateVars_Count(void) { Parse(); return sCount; }

const PortStateVar* PortStateVars_At(int index) {
    Parse();
    return (index >= 0 && index < sCount) ? &sVars[index] : NULL;
}

const PortStateVar* PortStateVars_Find(const char* name) {
    Parse();
    for (int i = 0; i < sCount; ++i)
        if (!strcmp(sVars[i].name, name)) return &sVars[i];
    return NULL;
}

const PortStateVar* PortStateVars_Containing(uintptr_t p, uint32_t* outOffset) {
    Parse();
    int lo = 0, hi = sCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const uintptr_t base = (uintptr_t)sVars[mid].ptr;
        if (p < base) hi = mid - 1;
        else if (p >= base + sVars[mid].size) lo = mid + 1;
        else {
            if (outOffset) *outOffset = (uint32_t)(p - base);
            return &sVars[mid];
        }
    }
    return NULL;
}
