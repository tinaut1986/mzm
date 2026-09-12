"""Proves the compiled WASM depth engine agrees with the native host build of
port_stereo_depth.c + port_cutscene_depth.c on the same input space -- the
guarantee the whole WASM-instead-of-JS-reimplementation approach exists for.

    python3 tools/layer-workbench/wasm/parity_check.py

Needs: depth_engine.js already built (wasm/build.sh), `node` on PATH, and a
C compiler (same TEST_CC the Makefile's `test` target uses, default `cc`) to
build a tiny native dumper. Not a reimplementation -- the real C driven from
both sides and diffed.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SRC = os.path.join(ROOT, "platform", "3ds", "source")
WORKBENCH = os.path.abspath(os.path.join(HERE, ".."))

CC = os.environ.get("TEST_CC", "cc")

# A sample runtime-override list, applied on BOTH sides for the rows after the
# OVERRIDES: line. SCENE is any valid PORT_CUT_SCENE_* id (8 == KRAID_RISING);
# OTHER a different one; LAYOUT_A/B two sub-scene signatures.
# Entry: (scene, layout, stage, target, tier). stage 0xFF == PORT_CUT_STAGE_ANY.
SCENE, OTHER = 8, 5
ANY = 0xFFFF
LAYOUT_A = (0) | (1 << 4) | (2 << 8) | (3 << 12)          # PORT_CUT_LAYOUT(0,1,2,3)
LAYOUT_B = (2) | (1 << 4) | (0xF << 8) | (0xF << 12)      # PORT_CUT_LAYOUT(2,1,OFF,OFF)
STAGE_ANY = 0xFF
OVERRIDES = [
    (SCENE, LAYOUT_A, STAGE_ANY, 0x01, 0),   # PRIO(1) -> BG_FAR, only in layout A
    (SCENE, LAYOUT_B, STAGE_ANY, 0x01, 3),   # PRIO(1) -> BG_OVERLAY, only in layout B
    (SCENE, ANY,      STAGE_ANY, 0x12, 1),   # BG(2)   -> BG_MID, any sub-scene
    (SCENE, ANY,      2,         0x21, 5),   # CAPTION -> OBJ_HUD, only on stage 2
]

NATIVE_DUMPER = r"""
#include "port_stereo_depth.h"
#include "port_cutscene_depth.h"
#include <stdio.h>
#include <string.h>

/* csv: fn,spread,p0,p1,p2,p3,inGameplay,bg0Overlay,samusOnTop,flatMenu,
 *      flatBackdropPrio,cutsceneArt,cutsceneScene,cutsceneLayout,arg,tier,pxMilli */
static void row(const char* fn, int spread, const PortStereoDepthState* st,
                int arg, int tier) {
    printf("%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%u,%d,%d,%d\n",
        fn, spread, st->priority[0], st->priority[1], st->priority[2], st->priority[3],
        st->inGameplay, st->bg0IsOverlayText, st->samusOnTopOfBackgrounds,
        st->flatMenu, st->flatMenuBackdropPrio, st->cutsceneArt, st->cutsceneScene,
        st->cutsceneLayout, arg, tier,
        (int)(PortStereoDepth_TierPxFor(spread, tier) * 1000));
}
static void dumpState(int spread, const PortStereoDepthState* st) {
    for (int bg = 0; bg < 4; bg++) row("bg", spread, st, bg, PortStereoDepth_BgTier(st, bg));
    for (int q = 0; q < 4; q++)   row("obj", spread, st, q, PortStereoDepth_ObjTier(st, q));
}

int main(void) {
    for (int spread = 0; spread < PORT_STEREO_SPREAD_COUNT; spread++) {
        for (unsigned packed = 0; packed < 256u; packed++) {
            PortStereoDepthState st;
            memset(&st, 0, sizeof(st));
            st.inGameplay = 1;
            for (int bg = 0; bg < 4; bg++) st.priority[bg] = (packed >> (bg*2)) & 3u;
            dumpState(spread, &st);

            /* cutsceneArt with no override list -> the built-in spread, a few
             * (scene, layout) pairs incl. 0 and the one overridden later. */
            PortStereoDepthState cs;
            memset(&cs, 0, sizeof(cs));
            cs.cutsceneArt = 1;
            for (int bg = 0; bg < 4; bg++) cs.priority[bg] = (packed >> (bg*2)) & 3u;
            static const int ids[] = {0, 1, __SCENE__};
            static const unsigned lays[] = {0u, __LAYOUT_A__, __LAYOUT_B__};
            for (int i = 0; i < 3; i++) {
                cs.cutsceneScene = (unsigned char)ids[i];
                cs.cutsceneLayout = (unsigned short)lays[i];
                dumpState(spread, &cs);
            }
        }
    }

    for (int flag = 0; flag < 2; flag++) {
        PortStereoDepthState st;
        memset(&st, 0, sizeof(st));
        st.inGameplay = 1;
        st.samusOnTopOfBackgrounds = flag;
        st.priority[0] = 2; st.priority[1] = 0; st.priority[2] = 1; st.priority[3] = 3;
        for (int bg = 0; bg < 4; bg++) row("bg", 0, &st, bg, PortStereoDepth_BgTier(&st, bg));
    }
    for (int split = 2; split <= 3; split++) {
        PortStereoDepthState st;
        memset(&st, 0, sizeof(st));
        st.flatMenu = 1;
        st.flatMenuBackdropPrio = split;
        st.priority[0] = 0; st.priority[1] = 1; st.priority[2] = 2; st.priority[3] = 3;
        for (int bg = 0; bg < 4; bg++) row("bg", 0, &st, bg, PortStereoDepth_BgTier(&st, bg));
    }

    /* Runtime override list active. The checker applies the same 5-byte
     * entries to the wasm engine when it sees the OVERRIDES: line (6 B). */
    printf("OVERRIDES:__ENTRIES_CSV__\n");
    {
        static const unsigned char entries[] = { __ENTRIES_BYTES__ };
        PortCutsceneDepth_SetRuntimeOverrides(entries, (int)(sizeof(entries) / 6));
    }
    for (int spread = 0; spread < PORT_STEREO_SPREAD_COUNT; spread++) {
        for (unsigned packed = 0; packed < 64u; packed++) {
            PortStereoDepthState cs;
            memset(&cs, 0, sizeof(cs));
            cs.cutsceneArt = 1;
            for (int bg = 0; bg < 4; bg++) cs.priority[bg] = (packed >> (bg*2)) & 3u;
            static const unsigned lays[] = {__LAYOUT_A__, __LAYOUT_B__, 0u};
            for (int li = 0; li < 3; li++) {
                cs.cutsceneScene = __SCENE__; cs.cutsceneLayout = (unsigned short)lays[li];
                dumpState(spread, &cs);
                cs.cutsceneScene = __OTHER__;  /* not the overridden scene */
                dumpState(spread, &cs);
            }
        }
    }
    return 0;
}
"""

_CHECKER_JS = r"""
const DepthEngineModule = require(process.argv[2]);
const fs = require("fs");

DepthEngineModule().then(M => {
    const T = Array(13).fill("number");
    const bgTier  = M.cwrap("depth_bg_tier",  "number", T);
    const objTier = M.cwrap("depth_obj_tier", "number", T);
    const tierPx  = M.cwrap("depth_tier_px_for", "number", ["number", "number"]);
    const setOverrides = M.cwrap("depth_cut_set_overrides", null, ["number"]);
    const scratchPtr   = M.cwrap("depth_cut_scratch", "number", []);

    const lines = fs.readFileSync(process.argv[3], "utf8").trim().split("\n").filter(Boolean);
    let checked = 0, mismatches = 0;
    for (const line of lines) {
        if (line.startsWith("OVERRIDES:")) {
            const entries = line.slice(10).split(";").filter(Boolean).map(s => s.split(",").map(Number));
            const buf = [];
            for (const [scene, layout, stage, target, tier] of entries)
                buf.push(scene, layout & 0xFF, (layout >> 8) & 0xFF, stage & 0xFF, target, tier & 0xFF);
            M.HEAPU8.set(Uint8Array.from(buf), scratchPtr());
            setOverrides(entries.length);
            continue;
        }
        const [fn, spread, p0, p1, p2, p3, inGameplay, bg0, samusTop, flat,
               flatPrio, cutsceneArt, cutsceneScene, cutsceneLayout,
               arg, expectTier, expectPxMilli] =
            line.split(",").map((v, i) => i === 0 ? v : Number(v));

        const a = [p0, p1, p2, p3, inGameplay, bg0, samusTop, flat, flatPrio,
                   cutsceneArt, cutsceneScene, cutsceneLayout, arg];
        const gotTier = fn === "bg" ? bgTier(...a) : objTier(...a);
        checked++;
        if (gotTier !== expectTier) {
            if (++mismatches <= 15)
                console.log(`MISMATCH ${fn}: prio=(${p0},${p1},${p2},${p3}) gp=${inGameplay} ` +
                            `cs=${cutsceneArt}/${cutsceneScene}/${cutsceneLayout} arg=${arg} ` +
                            `native=${expectTier} wasm=${gotTier}`);
            continue;
        }
        if (!isNaN(expectPxMilli)) {
            const gotMilli = Math.round(tierPx(spread, gotTier) * 1000);
            if (Math.abs(gotMilli - expectPxMilli) > 1 && ++mismatches <= 15)
                console.log(`PX MISMATCH ${fn}: tier=${gotTier} native=${expectPxMilli} wasm=${gotMilli}`);
        }
    }
    console.log(`${checked} checks, ${mismatches} mismatches`);
    process.exit(mismatches === 0 ? 0 : 1);
}).catch(e => { console.error(e); process.exit(1); });
"""


def main():
    tmp = tempfile.mkdtemp(prefix="depth_parity_")
    entries_bytes = ", ".join(
        str(b) for (sc, l, stg, t, tier) in OVERRIDES
        for b in (sc, l & 0xFF, (l >> 8) & 0xFF, stg & 0xFF, t, tier & 0xFF))
    entries_csv = ";".join(",".join(str(x) for x in e) for e in OVERRIDES)
    dumper_src = (NATIVE_DUMPER
                  .replace("__ENTRIES_BYTES__", entries_bytes)
                  .replace("__ENTRIES_CSV__", entries_csv)
                  .replace("__SCENE__", str(SCENE))
                  .replace("__OTHER__", str(OTHER))
                  .replace("__LAYOUT_A__", str(LAYOUT_A))
                  .replace("__LAYOUT_B__", str(LAYOUT_B)))

    dumper_c = os.path.join(tmp, "dump_native.c")
    dumper_bin = os.path.join(tmp, "dump_native")
    with open(dumper_c, "w") as f:
        f.write(dumper_src)

    subprocess.run([CC, "-std=c11", "-O1", "-Wall", "-I", SRC,
                    dumper_c,
                    os.path.join(SRC, "port_stereo_depth.c"),
                    os.path.join(SRC, "port_cutscene_depth.c"),
                    "-o", dumper_bin, "-lm"], check=True)
    native_out = subprocess.run([dumper_bin], check=True,
                                capture_output=True, text=True).stdout

    engine_js = os.path.join(WORKBENCH, "depth_engine.js")
    if not os.path.isfile(engine_js):
        sys.exit("depth_engine.js not built -- run tools/layer-workbench/wasm/build.sh first")

    checker = os.path.join(tmp, "check.js")
    with open(checker, "w") as f:
        f.write(_CHECKER_JS)
    dump_path = os.path.join(tmp, "native.csv")
    with open(dump_path, "w") as f:
        f.write(native_out)

    result = subprocess.run(["node", checker, engine_js, dump_path],
                            capture_output=True, text=True)
    print(result.stdout, end="")
    if result.returncode != 0:
        print(result.stderr, file=sys.stderr)
        sys.exit(result.returncode)


if __name__ == "__main__":
    main()
