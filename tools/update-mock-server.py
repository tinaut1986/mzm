#!/usr/bin/env python3
"""Local stand-in for the GitHub releases API, to test the 3DS self-updater.

Serves /releases.json in the same shape as
GET /repos/{owner}/{repo}/releases (newest first) plus the .cia files
themselves, so the console can be pointed at this machine instead of GitHub.

  # 1. serve a CIA and pretend it is release v9.9.9
  tools/update-mock-server.py --cia platform/3ds/mzm-3ds.cia --tag v9.9.9

  # 2. on the 3DS SD card, in mzm3ds.ini (same folder the game runs from):
  #      update_url=http://<this-machine-ip>:8000/releases.json
  #    then UPDATES > CHECK NOW (or relaunch with auto update on).

Add --beta-tag v9.9.10 to also publish a newer prerelease and exercise the
"releases + betas" channel. Every release carries a release body with a
"what's new" block between the same markers the real workflow writes, so the
UPDATES > WHAT'S NEW viewer can be tried too (--notes-file replaces the sample
text). Plain HTTP on purpose: it needs no certificate.
"""

import argparse
import json
import os
import socket
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("10.255.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


SAMPLE_NOTES = """- **Sample** first change, long enough that it has to wrap onto a second line of the viewer
- Second change with `code` and an accent: caf\u00e9
- Third change
- Fourth change
- Fifth change
- Sixth change
- Seventh change, so the list scrolls
- Eighth change"""


def release_body(tag, notes):
    """The same shape build-release.yml produces: notes block, then changelog."""
    return (
        "<!-- mzm-notes -->\n## What's new\n\n" + notes.replace("Sample", "Sample " + tag) +
        "\n<!-- /mzm-notes -->\n\n## Changelog\n\n- something (abc1234)\n"
    )


def make_handler(base, cia_path, releases, notes):
    body = json.dumps(
        [
            {
                "tag_name": tag,
                "prerelease": pre,
                "body": release_body(tag, notes),
                "assets": [
                    {
                        "name": "mzm-3ds.cia",
                        "browser_download_url": f"{base}/{tag}/mzm-3ds.cia",
                    }
                ],
            }
            for tag, pre in releases
        ]
    ).encode()
    size = os.path.getsize(cia_path)

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path.startswith("/releases.json"):
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif self.path.endswith("/mzm-3ds.cia"):
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.end_headers()
                try:
                    with open(cia_path, "rb") as f:
                        while chunk := f.read(65536):
                            self.wfile.write(chunk)
                except (BrokenPipeError, ConnectionResetError):
                    # The console hung up mid-download (it aborts on any
                    # installer error); that is its business, not ours.
                    print(f"[{self.client_address[0]}] client aborted the download")
            else:
                self.send_error(404)

        def log_message(self, fmt, *args):
            print(f"[{self.client_address[0]}] {fmt % args}")

    return Handler


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cia", required=True, help="CIA to serve for every release")
    ap.add_argument("--tag", default="v9.9.9", help="stable release tag to publish")
    ap.add_argument("--beta-tag", help="also publish this tag as a prerelease (listed first)")
    ap.add_argument("--notes-file", help="text for the notes block of every release (default: a sample)")
    ap.add_argument("--port", type=int, default=8000)
    args = ap.parse_args()

    notes = SAMPLE_NOTES
    if args.notes_file:
        with open(args.notes_file, encoding="utf-8") as f:
            notes = f.read().strip()
    base = f"http://{lan_ip()}:{args.port}"
    releases = []
    if args.beta_tag:
        releases.append((args.beta_tag, True))
    releases.append((args.tag, False))

    print(f"Serving {args.cia} as {[t for t, _ in releases]}")
    print(f"Set on the 3DS:  update_url={base}/releases.json")
    ThreadingHTTPServer(("0.0.0.0", args.port), make_handler(base, args.cia, releases, notes)).serve_forever()


if __name__ == "__main__":
    main()
