"""Proves the compiled WASM depth engine agrees with the native host test
binary (platform/3ds/tests/stereo_depth_test.c's own build of
port_stereo_depth.c) on the same input space -- the guarantee the whole
WASM-instead-of-JS-reimplementation approach exists for.

    python3 tools/layer-workbench/wasm/parity_check.py

Needs: depth_engine.js already built (wasm/build.sh), `node` on PATH, and a
C compiler (same TEST_CC the Makefile's `test` target uses, default `cc`) to
build a tiny native dumper that prints BgTier/ObjTier for the exact same
state space the JS side probes -- not a reimplementation, just the real
port_stereo_depth.c driven from both sides and diffed.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
SRC = os.path.join(ROOT, "platform", "3ds", "source")
WORKBENCH = os.path.abspath(os.path.join(HERE, ".."))

CC = os.environ.get("TEST_CC", "cc")

NATIVE_DUMPER = r"""
#include "port_stereo_depth.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    for (int spread = 0; spread < PORT_STEREO_SPREAD_COUNT; spread++) {
        for (unsigned packed = 0; packed < 256u; packed++) {
            PortStereoDepthState st;
            memset(&st, 0, sizeof(st));
            st.inGameplay = true;
            for (int bg = 0; bg < 4; bg++) st.priority[bg] = (packed >> (bg*2)) & 3u;
            for (int bg = 0; bg < 4; bg++)
                printf("bg,%d,%d,%d,%d,%d,%d,0,0,0,0,%d,%d,%d\n",
                    spread, st.priority[0], st.priority[1], st.priority[2], st.priority[3],
                    st.inGameplay, bg, PortStereoDepth_BgTier(&st, bg),
                    (int)(PortStereoDepth_TierPxFor(spread, PortStereoDepth_BgTier(&st, bg)) * 1000));
            for (int q = 0; q < 4; q++)
                printf("obj,%d,%d,%d,%d,%d,%d,0,0,0,0,%d,%d,%d\n",
                    spread, st.priority[0], st.priority[1], st.priority[2], st.priority[3],
                    st.inGameplay, q, PortStereoDepth_ObjTier(&st, q),
                    (int)(PortStereoDepth_TierPxFor(spread, PortStereoDepth_ObjTier(&st, q)) * 1000));
        }
    }
    /* samusOnTopOfBackgrounds and flatMenu corners, matching the host test's coverage. */
    for (int flag = 0; flag < 2; flag++) {
        PortStereoDepthState st;
        memset(&st, 0, sizeof(st));
        st.inGameplay = true;
        st.samusOnTopOfBackgrounds = flag;
        st.priority[0] = 2; st.priority[1] = 0; st.priority[2] = 1; st.priority[3] = 3;
        for (int bg = 0; bg < 4; bg++)
            printf("samus,0,%d,%d,%d,%d,1,0,%d,0,0,%d,%d,0\n",
                st.priority[0], st.priority[1], st.priority[2], st.priority[3], flag,
                bg, PortStereoDepth_BgTier(&st, bg));
    }
    for (int split = 2; split <= 3; split++) {
        PortStereoDepthState st;
        memset(&st, 0, sizeof(st));
        st.flatMenu = true;
        st.flatMenuBackdropPrio = split;
        st.priority[0] = 0; st.priority[1] = 1; st.priority[2] = 2; st.priority[3] = 3;
        for (int bg = 0; bg < 4; bg++)
            printf("flat,0,%d,%d,%d,%d,0,0,0,1,%d,%d,%d,0\n",
                st.priority[0], st.priority[1], st.priority[2], st.priority[3],
                split, bg, PortStereoDepth_BgTier(&st, bg));
    }
    return 0;
}
"""

def main():
    tmp = tempfile.mkdtemp(prefix="depth_parity_")
    dumper_c = os.path.join(tmp, "dump_native.c")
    dumper_bin = os.path.join(tmp, "dump_native")
    with open(dumper_c, "w") as f:
        f.write(NATIVE_DUMPER)

    subprocess.run([CC, "-std=c11", "-O1", "-Wall", "-I", SRC,
                     dumper_c, os.path.join(SRC, "port_stereo_depth.c"),
                     "-o", dumper_bin, "-lm"], check=True)
    native_out = subprocess.run([dumper_bin], check=True, capture_output=True, text=True).stdout

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


_CHECKER_JS = r"""
const DepthEngineModule = require(process.argv[2]);
const fs = require("fs");

DepthEngineModule().then(M => {
    const argT = ["number","number","number","number","number","number","number","number","number","number"];
    const bgTier = M.cwrap("depth_bg_tier", "number", argT);
    const objTier = M.cwrap("depth_obj_tier", "number", argT);
    const tierPx = M.cwrap("depth_tier_px_for", "number", ["number","number"]);

    const lines = fs.readFileSync(process.argv[3], "utf8").trim().split("\n").filter(Boolean);
    let checked = 0, mismatches = 0;
    for (const line of lines) {
        const parts = line.split(",").map((v, i) => i === 0 ? v : Number(v));
        const [kind, spread, p0, p1, p2, p3, inGameplay, bg0Overlay, samusOnTop,
               flatMenu, flatMenuBackdropPrio, arg, expectTier, expectPxMilli] = parts;

        let gotTier;
        if (kind === "bg" || kind === "samus" || kind === "flat") {
            gotTier = bgTier(p0, p1, p2, p3, inGameplay, bg0Overlay, samusOnTop, flatMenu, flatMenuBackdropPrio, arg);
        } else if (kind === "obj") {
            gotTier = objTier(p0, p1, p2, p3, inGameplay, bg0Overlay, samusOnTop, flatMenu, flatMenuBackdropPrio, arg);
        } else {
            continue;
        }
        checked++;
        if (gotTier !== expectTier) {
            mismatches++;
            if (mismatches <= 10) {
                console.log(`MISMATCH ${kind}: state=(${p0},${p1},${p2},${p3},gp=${inGameplay}) arg=${arg} native=${expectTier} wasm=${gotTier}`);
            }
            continue;
        }
        if ((kind === "bg" || kind === "obj") && expectPxMilli !== undefined && !isNaN(expectPxMilli)) {
            const px = tierPx(spread, gotTier);
            const gotMilli = Math.round(px * 1000);
            if (Math.abs(gotMilli - expectPxMilli) > 1) {
                mismatches++;
                if (mismatches <= 10) {
                    console.log(`PX MISMATCH ${kind}: tier=${gotTier} native=${expectPxMilli} wasm=${gotMilli}`);
                }
            }
        }
    }
    console.log(`${checked} checks, ${mismatches} mismatches`);
    process.exit(mismatches === 0 ? 0 : 1);
}).catch(e => { console.error(e); process.exit(1); });
"""

if __name__ == "__main__":
    main()
