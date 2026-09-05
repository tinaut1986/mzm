#pragma once

/*
 * The one gate for every 3DS debug facility: the bottom screen's
 * DEBUG -> HERRAMIENTAS menu and the tools behind it (instant-kill, scene
 * recorder, perf recorder, 16x16 block pass toggle, live atlas dump,
 * one-shot screen/state dump, USER MARK log line, room warp, equipment) AND
 * every throttled per-frame diagnostic log stream (GPUDIAG/GPUTIME/PBFLASH,
 * audio traces, PERF timing -- see docs/3ds-debug-tools.md).
 *
 * All of it is compiled out entirely -- not just runtime-disabled -- unless
 * PORT_DEBUG_TOOLS is defined, i.e. unless the build is:
 *
 *   make clean && make DEBUG_TOOLS=1
 *
 * so a production build doesn't even draw the menu button, let alone carry
 * the per-frame logging code or a path to any action behind the menu.
 *
 * There used to be three separate opt-in flags -- PORT_DEBUG_TOOLS for the
 * menu, PORT_GPU_RENDERER_DIAG_LOG and PORT_AUDIO_DIAG_LOG (and an ad-hoc
 * PORT_PPU_PERF_LOG) for the logs, each its own EXTRA_CFLAGS. They are gone:
 * a DEBUG_TOOLS=1 build compiles in every stream and you pick which one(s)
 * actually write from the menu (LOG mode: OFF / ALL / GPU / AUDIO / PERF --
 * see port_debug_log.h). Decompiled src/ files that can't include this
 * header check `defined(PORT_DEBUG_TOOLS)` directly instead.
 */
#if defined(PORT_DEBUG_TOOLS)
#define PORT_DEBUG_TOOLS_ACTIVE 1
#endif
