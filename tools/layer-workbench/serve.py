"""Abre el banco de capas en el navegador, con los mapas ya cargados.

    python3 tools/layer-workbench/serve.py   (python, en Windows)

Sobre file:// el navegador no deja leer maps.json (mismo origen), así que hay
que elegirlo a mano cada vez. Sirviéndolo por http el visor lo carga solo, y
esto vale igual en Linux y en Windows, que es donde se alterna.

Genera maps.json si no está o si los datos del juego son más nuevos.

También hace de puente con el archivo de correcciones: el visor lo carga solo
al abrir y lo guarda solo al cambiar, en la ruta donde el port lo compila
(platform/3ds/source/port_layer_fixes.inc). Sobre file:// eso no es posible --
el navegador no escribe en disco sin diálogo -- y hay que usar los botones de
CARGAR y GUARDAR.
"""
import http.server
import os
import subprocess
import sys
import threading
import webbrowser

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PORT = 8731


def newest(*paths):
    t = 0
    for p in paths:
        if os.path.isfile(p):
            t = max(t, os.path.getmtime(p))
        for base, _, files in os.walk(p) if os.path.isdir(p) else ():
            for f in files:
                t = max(t, os.path.getmtime(os.path.join(base, f)))
    return t


def link_worktree_assets():
    """Si estamos en un worktree secundario, enlaza recursos extraídos del repo principal."""
    try:
        common_git = subprocess.check_output(
            ["git", "rev-parse", "--git-common-dir"],
            cwd=ROOT,
            stderr=subprocess.DEVNULL,
            text=True
        ).strip()
        main_root = os.path.abspath(os.path.join(common_git, ".."))
        if os.path.isdir(main_root) and main_root != ROOT:
            for item in ("include/extracted", "data/rooms", "data/tilesets"):
                src_item = os.path.join(main_root, item.replace("/", os.sep))
                dst_item = os.path.join(ROOT, item.replace("/", os.sep))
                if os.path.exists(src_item) and not os.path.exists(dst_item):
                    os.makedirs(os.path.dirname(dst_item), exist_ok=True)
                    try:
                        if hasattr(os, "symlink"):
                            os.symlink(src_item, dst_item)
                        else:
                            import shutil
                            shutil.copytree(src_item, dst_item)
                    except Exception:
                        pass
    except Exception:
        pass


def ensure_maps():
    link_worktree_assets()
    maps = os.path.join(HERE, "maps.json")
    sources = newest(os.path.join(ROOT, "src", "data", "rooms_data.c"),
                     os.path.join(ROOT, "data", "rooms"),
                     os.path.join(ROOT, "data", "tilesets"),
                     os.path.join(HERE, "build_maps.py"))
    if os.path.isfile(maps) and os.path.getmtime(maps) >= sources:
        return
    print("generando maps.json...")
    subprocess.run([sys.executable, os.path.join(HERE, "build_maps.py")], check=True)


def ensure_sprites():
    sprites = os.path.join(HERE, "sprites.json")
    sources = newest(os.path.join(ROOT, "include", "constants", "sprite.h"),
                     os.path.join(ROOT, "platform", "3ds", "source", "port_stereo_depth.h"),
                     os.path.join(ROOT, "platform", "3ds", "source", "port_sprite_depth_oam.h"),
                     os.path.join(HERE, "build_sprites.py"))
    stale = not os.path.isfile(sprites) or os.path.getmtime(sprites) < sources
    if stale:
        print("generando sprites.json...")
        subprocess.run([sys.executable, os.path.join(HERE, "build_sprites.py")], check=True)
    ensure_sprite_thumbs(rebuilt=stale)


def ensure_sprite_thumbs(rebuilt):
    """Sprite thumbnails. Rebuilt when sprites.json is newer, when the repo's
    sprite data changed (src/sprite.c, src/data/sprites/, the extracted .inc
    files) or when the generator itself changed."""
    thumbs = os.path.join(HERE, "thumbs")
    marker = os.path.join(thumbs, ".built")
    sources = newest(os.path.join(HERE, "sprites.json"),
                     os.path.join(HERE, "build_sprite_thumbs.py"),
                     os.path.join(ROOT, "src", "sprite.c"),
                     os.path.join(ROOT, "src", "data", "sprites"),
                     os.path.join(ROOT, "include", "extracted", "data", "sprites"))
    if not rebuilt and os.path.isfile(marker) and os.path.getmtime(marker) >= sources:
        return
    print("generando miniaturas de sprite...")
    try:
        subprocess.run([sys.executable, os.path.join(HERE, "build_sprite_thumbs.py")],
                       check=True)
        os.makedirs(thumbs, exist_ok=True)
        open(marker, "w").close()
    except subprocess.CalledProcessError as e:
        # Without thumbnails, SPRITES mode still works as a plain catalogue.
        print("  (fallo, sigo sin miniaturas: %s)" % e)


FIXES = os.path.join(ROOT, "platform", "3ds", "source", "port_layer_fixes.inc")
SPRITE_FIXES = os.path.join(ROOT, "platform", "3ds", "source", "port_sprite_depth.inc")


class Handler(http.server.SimpleHTTPRequestHandler):
    """Lo de siempre, más /fixes y /sprite-fixes para leer y escribir los .inc.

    Sólo escucha en 127.0.0.1 y sólo escribe en esas dos rutas concretas: no
    es un servidor de archivos general con escritura.
    """

    # ruta -> (fichero, marca X-macro para el recuento del log)
    ENDPOINTS = {
        "/fixes": (FIXES, b"PORT_LAYER_FIX("),
        "/sprite-fixes": (SPRITE_FIXES, b"PORT_SPRITE_DEPTH("),
    }

    def end_headers(self):
        # SimpleHTTPRequestHandler serves index.html with only Last-Modified,
        # which browsers heuristically cache -- so after editing index.html the
        # change shows over file:// but NOT over http until a manual
        # hard-refresh. Force revalidation on every response (this hook runs
        # for both the static handler and _send).
        self.send_header("Cache-Control", "no-store, must-revalidate")
        super().end_headers()

    def _send(self, code, body=b"", ctype="text/plain; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        ep = self.ENDPOINTS.get(self.path.split("?")[0])
        if ep:
            path = ep[0]
            if not os.path.isfile(path):
                return self._send(404, "sin correcciones todavía".encode("utf-8"))
            with open(path, "rb") as f:
                return self._send(200, f.read())
        return http.server.SimpleHTTPRequestHandler.do_GET(self)

    def do_PUT(self):
        ep = self.ENDPOINTS.get(self.path.split("?")[0])
        if not ep:
            return self._send(405, b"solo /fixes y /sprite-fixes")
        path, mark = ep
        n = int(self.headers.get("Content-Length") or 0)
        if n > (1 << 20):
            return self._send(413, b"demasiado grande")
        data = self.rfile.read(n)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        # Atómico: un guardado a medias dejaría al port compilando una lista
        # truncada.
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)
        print("guardadas %d entradas en %s"
              % (data.count(mark), os.path.relpath(path, ROOT)))
        return self._send(200, b"ok")

    def log_message(self, fmt, *args):
        pass          # el ruido de cada GET no aporta nada


def main():
    ensure_maps()
    ensure_sprites()
    os.chdir(HERE)
    # Sólo desde esta máquina: sirve el repo y escribe en él, no hay por qué
    # exponerlo. Si el puerto está pillado -- otra copia abierta -- se prueba el
    # siguiente en vez de morir con un traceback.
    server = None
    for port in range(PORT, PORT + 20):
        try:
            server = http.server.ThreadingHTTPServer(("127.0.0.1", port), Handler)
            break
        except OSError:
            continue
    if server is None:
        sys.exit("no hay ningún puerto libre entre %d y %d" % (PORT, PORT + 19))

    url = "http://127.0.0.1:%d/index.html" % port
    print("banco de capas en " + url + "   (Ctrl+C para parar)")
    print("correcciones capas   " + os.path.relpath(FIXES, ROOT))
    print("correcciones sprites " + os.path.relpath(SPRITE_FIXES, ROOT))
    threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
