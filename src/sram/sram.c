#include "sram/sram.h"

#include "gba.h"
#include "io.h"

static const char sSramVersion[] = "SRAM_V113";

#if defined(MZM_3DS) || defined(PORT_NATIVE)
#include <string.h>
#include <stdio.h>
#include "port_gba_mem.h"
#ifdef MZM_3DS
#include "port_debug_log.h"
#endif

#define SAVE_FILE_PATH "mzm.sav"

/* Writes to SD are far too slow to run once per SramWrite* call (a single
 * in-game save fans out into dozens of them via unk_fbc / SramSaveFile's
 * staged state machine, and each one used to rewrite the whole 64 KB file
 * synchronously -- the source of the save-point frame hitches, issue #22).
 * Every write only touches gSramMem and marks it dirty. Once per frame
 * Port_FlushSramIfDirty() snapshots gSramMem and hands it to a dedicated
 * save thread that does the SD write in the background -- safe because the
 * game keeps Samus frozen for several seconds during the save animation.
 * Port_FlushSramWait() blocks until everything is on disk (shutdown path). */
#ifdef MZM_3DS
#include <pthread.h>

static u8 sSramSnapshot[0x10000]; /* same size as gSramMem */
static u8 sSramDirty = FALSE;          /* set by SramWrite*, game thread */
static u8 sSnapshotPending = FALSE;    /* snapshot awaiting write, save thread */
static u8 sShutdown = FALSE;
static pthread_mutex_t sSaveMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sSaveCond = PTHREAD_COND_INITIALIZER;
static pthread_t sSaveThread;
static u8 sSaveThreadStarted = FALSE;

static void SramWriteAtomic(const u8* data, u32 size);

static void* SaveThreadMain(void* arg)
{
    (void)arg;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, NULL);
    for (;;)
    {
        pthread_mutex_lock(&sSaveMutex);
        while (!sSnapshotPending && !sShutdown)
            pthread_cond_wait(&sSaveCond, &sSaveMutex);
        if (!sSnapshotPending && sShutdown)
        {
            pthread_mutex_unlock(&sSaveMutex);
            break;
        }
        /* Consume the snapshot BEFORE releasing the lock: from this moment
         * the producer is allowed to refill the buffer, but only once it
         * sees pending==FALSE, which we only publish after the SD write has
         * fully completed below. This closes the race where the producer
         * refilled (and tore) the snapshot mid-fwrite and the lost update
         * silently left stale data on disk. */
        pthread_mutex_unlock(&sSaveMutex);

        SramWriteAtomic(sSramSnapshot, sizeof(sSramSnapshot));
        Port_DebugLog("Port_SaveSram: wrote mzm.sav");

        pthread_mutex_lock(&sSaveMutex);
        sSnapshotPending = FALSE;
        pthread_cond_broadcast(&sSaveCond);
        pthread_mutex_unlock(&sSaveMutex);
    }
    return NULL;
}

static void SaveThreadStart(void)
{
    if (!sSaveThreadStarted)
    {
        sSaveThreadStarted = TRUE;
        if (pthread_create(&sSaveThread, NULL, SaveThreadMain, NULL) != 0)
            sSaveThreadStarted = FALSE;
    }
}
#endif

void Port_LoadSram(void)
{
    FILE* f = fopen(SAVE_FILE_PATH, "rb");
    if (!f)
    {
        /* Main save missing but a complete .tmp survived a crash between
         * remove() and rename()? Recover it instead of starting blank. */
        f = fopen(SAVE_FILE_PATH ".tmp", "rb");
        if (!f)
            return;
    }
    fread(gSramMem, 1, sizeof(gSramMem), f);
    fclose(f);
}

/* Atomic save write: the data lands in a temp file first and only replaces
 * mzm.sav via rename once fully written. A power loss mid-write can then
 * never leave a truncated/zeroed mzm.sav (issue #22 follow-up: a "wb" open
 * truncates immediately, so dying mid-fwrite looked like a wiped save). */
static void SramWriteAtomic(const u8* data, u32 size)
{
    FILE* f = fopen(SAVE_FILE_PATH ".tmp", "wb");
    if (!f)
        return;
    if (fwrite(data, 1, size, f) != size)
    {
        fclose(f);
        remove(SAVE_FILE_PATH ".tmp");
        return;
    }
    fclose(f);
    /* FatFs rename fails if the destination exists -- swap by hand. The
     * window between remove() and rename() is two metadata ops wide; the
     * .tmp file itself is always complete, so worst case after a crash here
     * is recovering it manually. */
    remove(SAVE_FILE_PATH);
    if (rename(SAVE_FILE_PATH ".tmp", SAVE_FILE_PATH) != 0)
    {
        /* rename can legitimately fail on some filesystems if dest exists;
         * we already removed it above, so this should not happen. */
    }
}

void Port_SaveSram(void)
{
    SramWriteAtomic(gSramMem, sizeof(gSramMem));
#ifdef MZM_3DS
    Port_DebugLog("Port_SaveSram: wrote mzm.sav");
#endif
}

void Port_FlushSramIfDirty(void)
{
    if (!sSramDirty)
        return;
    sSramDirty = FALSE;
#ifdef MZM_3DS
    SaveThreadStart();
    if (sSaveThreadStarted)
    {
        pthread_mutex_lock(&sSaveMutex);
        /* Never refill while the consumer is still writing the previous
         * snapshot out to SD: overwriting sSramSnapshot mid-fwrite both tore
         * the file being written and lost the newest update when the
         * consumer then cleared the single pending flag. Blocking here is
         * bounded by one SD write (~tens of ms) and only happens if saves
         * come faster than the drive -- which the staged save machine never
         * does two frames in a row. */
        while (sSnapshotPending)
            pthread_cond_wait(&sSaveCond, &sSaveMutex);
        memcpy(sSramSnapshot, gSramMem, sizeof(gSramMem));
        sSnapshotPending = TRUE;
        pthread_cond_signal(&sSaveCond);
        pthread_mutex_unlock(&sSaveMutex);
        /* Piggyback diagnostics on the (rare) SD write instead of per frame. */
        Port_DebugLogFlush();
        return;
    }
#endif
    Port_SaveSram();
}

void Port_FlushSramWait(void)
{
#ifdef MZM_3DS
    Port_FlushSramIfDirty();
    if (!sSaveThreadStarted)
        return;
    pthread_mutex_lock(&sSaveMutex);
    while (sSnapshotPending)
        pthread_cond_wait(&sSaveCond, &sSaveMutex);
    pthread_mutex_unlock(&sSaveMutex);
    /* Make sure diagnostics from this session reach the SD before exit. */
    Port_DebugLogFlush();
#else
    if (sSramDirty)
    {
        sSramDirty = FALSE;
        Port_SaveSram();
    }
#endif
}

void SramWriteUnchecked(u8* src, u8* dest, u32 size)
{
    void* d = port_resolve_write_addr((uintptr_t)dest);
    const void* s = port_resolve_copy_src(src, size);
    if (d && s) memcpy(d, s, size);
    sSramDirty = TRUE;
}

void SramWrite(u8* src, u8* dest, u32 size)
{
    void* d = port_resolve_write_addr((uintptr_t)dest);
    const void* s = port_resolve_copy_src(src, size);
    if (d && s) memcpy(d, s, size);
    sSramDirty = TRUE;
}

u8* SramCheck(u8* src, u8* dest, u32 size)
{
    u32 i;
    void* d = port_resolve_write_addr((uintptr_t)dest);
    const void* s = port_resolve_copy_src(src, size);
    if (d && s) {
        u8* d8 = (u8*)d;
        const u8* s8 = (const u8*)s;
        for (i = 0; i < size; i++) {
            if (d8[i] != s8[i]) return dest + i;
        }
    }
    return NULL;
}

u8* SramWriteChecked(u8* src, u8* dest, u32 size)
{
    SramWrite(src, dest, size);
    return SramCheck(src, dest, size);
}


#else

static void SramWriteUncheckedInternal(u8* src, u8* dest, u32 size)
{
    while (size-- != 0)
        *dest++ = *src++;
}

void SramWriteUnchecked(u8* src, u8* dest, u32 size)
{
    u16 code[0x40];
    u16* code_ptr;
    u16* func_ptr;
    u16 csize;
    void* (*func)(u8*, u8*, u32);

    WRITE_16(REG_WAITCNT, READ_16(REG_WAITCNT) & ~WAIT_SRAM_CYCLES_MASK | WAIT_SRAM_8CYCLES);

    func_ptr = (u16*)SramWriteUncheckedInternal;
    func_ptr = (u16*)((u32)func_ptr & ~1);
    code_ptr = code;

    for (csize = ((u32)SramWriteUnchecked - (u32)SramWriteUncheckedInternal) / 2; csize > 0; --csize)
        *code_ptr++ = *func_ptr++;

    func = (void* (*)(u8*, u8*, u32))code + 1;
    func(src, dest, size);
}

void SramWrite(u8* src, u8* dest, u32 size)
{
    u16 w = READ_16(REG_WAITCNT) & ~WAIT_SRAM_CYCLES_MASK | WAIT_SRAM_8CYCLES;
    WRITE_16(REG_WAITCNT, w);

    while (size-- != 0)
        *dest++ = *src++;
}

static u8* SramCheckInternal(u8* src, u8* dest, u32 size)
{
    while (size-- != 0)
    {
        if (*dest++ != *src++)
            return dest - 1;
    }

    return NULL;
}

u8* SramCheck(u8* src, u8* dest, u32 size)
{
    u16 code[0x60];
    u16* code_ptr;
    u16* func_ptr;
    u16 csize;
    void* (*func)(u8*, u8*, u32);

    WRITE_16(REG_WAITCNT, READ_16(REG_WAITCNT) & ~WAIT_SRAM_CYCLES_MASK | WAIT_SRAM_8CYCLES);

    func_ptr = (u16*)SramCheckInternal;
    func_ptr = (u16*)((u32)func_ptr & ~1);
    code_ptr = code;

    for (csize = ((u32)SramCheck - (u32)SramCheckInternal) / 2; csize > 0; --csize)
        *code_ptr++ = *func_ptr++;

    func = (void* (*)(u8*, u8*, u32))code + 1;
    return func(src, dest, size);
}

u8* SramWriteChecked(u8* src, u8* dest, u32 size)
{
    u8* diff;
    u8 i;

#ifdef BUGFIX
    diff = NULL;
#endif // BUGFIX

    for (i = 0; i < 3; ++i)
    {
        SramWrite(src, dest, size);
        diff = SramCheck(src, dest, size);
        if (!diff)
            break;
    }

    return diff;
}
#endif

