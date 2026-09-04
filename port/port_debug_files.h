#ifndef PORT_DEBUG_FILES_H
#define PORT_DEBUG_FILES_H

/* Slot rotation for the on-device debug captures.
 *
 * Every capture tool used to write one fixed filename, overwritten in place
 * on every use: a second perf capture, screen dump or recording destroyed
 * the previous one, so comparing two runs meant fetching the file over FTP
 * between them and hoping you remembered to. (The scene recorder was the
 * exception and went the other way: it appended -2, -3 ... up to -99, i.e.
 * up to ~800MB of 8MB samples, with nothing ever pruned.)
 *
 * These helpers keep the last `keep` captures of a given kind and reuse the
 * oldest slot once that many exist:
 *
 *   mzm-perf-01.bin ... mzm-perf-10.bin
 *
 * Slot numbers are NOT a chronological order -- the newest capture is
 * whichever slot was free or least recently written. Order a fetched set by
 * modification time (an FTP listing shows it) rather than by name.
 *
 * Renaming to keep slot 01 as "newest", logrotate style, was considered and
 * rejected: it would mean rewriting up to `keep` files on every capture,
 * which for the 8MB scene recordings is worse than the problem it solves.
 */

#include <stdbool.h>
#include <stddef.h>

/* Path of the slot the NEXT capture should be written to: the first free
 * one, or the least recently modified if all `keep` are taken. Composes
 * "<PORT_DEBUG_FILES_DIR>/<name>-NN<ext>". `keep` is clamped to 1..99.
 * Returns false (and leaves `out` empty) only if the path would not fit. */
bool Port_DebugFiles_NextPath(const char* name, const char* ext, unsigned keep,
                              char* out, size_t outLen);

/* Same slot choice, but returns just the number, for a capture that spreads
 * over several files sharing one index (the screen dump writes eleven).
 * `probeSuffix` is the part after the number for the ONE file used to test
 * whether a slot is taken -- pass the suffix of a file the capture always
 * writes, e.g. "-left.rgb" for "mzm-dump-03-left.rgb". Returns 1 if
 * nothing can be probed. */
unsigned Port_DebugFiles_NextSetIndex(const char* name, const char* probeSuffix,
                                      unsigned keep);

/* Composes "<PORT_DEBUG_FILES_DIR>/<name>-NN<suffix>" for an index already
 * obtained from Port_DebugFiles_NextSetIndex. */
bool Port_DebugFiles_SetPath(const char* name, unsigned index, const char* suffix,
                             char* out, size_t outLen);

#endif /* PORT_DEBUG_FILES_H */
