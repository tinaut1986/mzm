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


def ensure_maps():
    maps = os.path.join(HERE, "maps.json")
    sources = newest(os.path.join(ROOT, "src", "data", "rooms_data.c"),
                     os.path.join(ROOT, "data", "rooms"),
                     os.path.join(ROOT, "data", "tilesets"),
                     os.path.join(HERE, "build_maps.py"))
    if os.path.isfile(maps) and os.path.getmtime(maps) >= sources:
        return
    print("generando maps.json...")
    subprocess.run([sys.executable, os.path.join(HERE, "build_maps.py")], check=True)


FIXES = os.path.join(ROOT, "platform", "3ds", "source", "port_layer_fixes.inc")


class Handler(http.server.SimpleHTTPRequestHandler):
    """Lo de siempre, más /fixes para leer y escribir el .inc.

    Sólo escucha en 127.0.0.1 y sólo escribe en esa ruta concreta: no es un
    servidor de archivos general con escritura.
    """

    def _send(self, code, body=b"", ctype="text/plain; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        if self.path.split("?")[0] == "/fixes":
            if not os.path.isfile(FIXES):
                return self._send(404, "sin correcciones todavía".encode("utf-8"))
            with open(FIXES, "rb") as f:
                return self._send(200, f.read())
        return http.server.SimpleHTTPRequestHandler.do_GET(self)

    def do_PUT(self):
        if self.path.split("?")[0] != "/fixes":
            return self._send(405, b"solo /fixes")
        n = int(self.headers.get("Content-Length") or 0)
        if n > (1 << 20):
            return self._send(413, b"demasiado grande")
        data = self.rfile.read(n)
        os.makedirs(os.path.dirname(FIXES), exist_ok=True)
        # Atómico: un guardado a medias dejaría al port compilando una lista
        # truncada.
        tmp = FIXES + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, FIXES)
        print("guardadas %d correcciones en %s"
              % (data.count(b"PORT_LAYER_FIX("), os.path.relpath(FIXES, ROOT)))
        return self._send(200, b"ok")

    def log_message(self, fmt, *args):
        pass          # el ruido de cada GET no aporta nada


def main():
    ensure_maps()
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
    print("correcciones en    " + os.path.relpath(FIXES, ROOT))
    threading.Timer(0.4, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print()


if __name__ == "__main__":
    main()
