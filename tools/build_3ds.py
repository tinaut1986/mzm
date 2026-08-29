#!/usr/bin/env python3
"""Script de compilación y despliegue por FTP para Metroid Zero Mission 3DS.

Soporta ejecución no interactiva por parámetros y menú interactivo navegable
con flechas de dirección.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

# Rutas base
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, ".."))
PLATFORM_3DS = os.path.join(ROOT, "platform", "3ds")
IP_HISTORY_FILE = os.path.join(PLATFORM_3DS, ".3ds_ftp_ip")

# Códigos ANSI para colores y formato
RESET = "\033[0m"
BOLD = "\033[1m"
DIM = "\033[2m"
CYAN = "\033[36m"
GREEN = "\033[32m"
YELLOW = "\033[33m"
RED = "\033[31m"
MAGENTA = "\033[35m"
BG_BLUE = "\033[44m"
BG_CYAN = "\033[46m"
WHITE = "\033[37m"
CLEAR_LINE = "\033[2K"
CURSOR_UP = "\033[F"
HIDE_CURSOR = "\033[?25l"
SHOW_CURSOR = "\033[?25h"


def supports_color():
    """Comprueba si la consola soporta colores ANSI."""
    if not sys.stdout.isatty():
        return False
    if os.name == "nt":
        # Windows 10+ soporta ANSI si VT100 está activado
        return True
    return os.environ.get("TERM") != "dumb"


class RawTerminal:
    """Manejo multiplataforma de lectura de teclado en tiempo real."""

    def __enter__(self):
        self.old_settings = None
        if os.name != "nt" and sys.stdin.isatty():
            import termios
            import tty
            self.fd = sys.stdin.fileno()
            self.old_settings = termios.tcgetattr(self.fd)
            # setcbreak desactiva el buffer de línea y el eco pero conserva la
            # traducción de saltos de línea (\n -> \r\n), evitando el efecto escalera
            tty.setcbreak(self.fd)
        sys.stdout.write(HIDE_CURSOR)
        sys.stdout.flush()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        sys.stdout.write(SHOW_CURSOR)
        sys.stdout.flush()
        if os.name != "nt" and self.old_settings is not None:
            import termios
            termios.tcsetattr(self.fd, termios.TCSADRAIN, self.old_settings)

    def get_key(self):
        """Lee una pulsación de tecla y la normaliza."""
        if os.name == "nt":
            import msvcrt
            ch = msvcrt.getch()
            if ch in (b"\x00", b"\xe0"):
                ch2 = msvcrt.getch()
                if ch2 == b"H":
                    return "UP"
                elif ch2 == b"P":
                    return "DOWN"
                elif ch2 == b"K":
                    return "LEFT"
                elif ch2 == b"M":
                    return "RIGHT"
                return "OTHER"
            elif ch in (b"\r", b"\n"):
                return "ENTER"
            elif ch == b"\x1b":
                return "ESC"
            elif ch == b"\x03":
                raise KeyboardInterrupt
            elif ch in (b"q", b"Q"):
                return "QUIT"
            elif ch in (b"w", b"W", b"k", b"K"):
                return "UP"
            elif ch in (b"s", b"S", b"j", b"J"):
                return "DOWN"
            elif ch == b" ":
                return "SPACE"
            return ch.decode("utf-8", errors="ignore")
        else:
            import select
            data = os.read(self.fd, 32)
            if not data:
                return "OTHER"
            if data in (b"\x1b[A", b"\x1bOA"):
                return "UP"
            elif data in (b"\x1b[B", b"\x1bOB"):
                return "DOWN"
            elif data in (b"\x1b[C", b"\x1bOC"):
                return "RIGHT"
            elif data in (b"\x1b[D", b"\x1bOD"):
                return "LEFT"
            elif data.startswith(b"\x1b[") or data.startswith(b"\x1bO"):
                if data.endswith(b"A"):
                    return "UP"
                elif data.endswith(b"B"):
                    return "DOWN"
                elif data.endswith(b"C"):
                    return "RIGHT"
                elif data.endswith(b"D"):
                    return "LEFT"
                return "OTHER"
            elif data == b"\x1b":
                # Si llega solo ESC, esperar brevemente por si es el prefijo de una secuencia lenta
                r, _, _ = select.select([self.fd], [], [], 0.05)
                if r:
                    more = os.read(self.fd, 31)
                    full = data + more
                    if full.endswith(b"A"):
                        return "UP"
                    elif full.endswith(b"B"):
                        return "DOWN"
                    elif full.endswith(b"C"):
                        return "RIGHT"
                    elif full.endswith(b"D"):
                        return "LEFT"
                    return "OTHER"
                return "ESC"
            elif data in (b"\r", b"\n"):
                return "ENTER"
            elif data == b"\x03":
                raise KeyboardInterrupt
            elif data in (b"q", b"Q"):
                return "QUIT"
            elif data in (b"w", b"W", b"k", b"K"):
                return "UP"
            elif data in (b"s", b"S", b"j", b"J"):
                return "DOWN"
            elif data == b" ":
                return "SPACE"
            return data.decode("utf-8", errors="ignore")


def clear_screen():
    if os.name == "nt":
        os.system("cls")
    else:
        sys.stdout.write("\033[2J\033[H")
        sys.stdout.flush()


def interactive_select(title, options, default_index=0, subtitle=None):
    """Muestra un menú interactivo navegable con flechas."""
    current = default_index
    num_options = len(options)

    with RawTerminal() as term:
        while True:
            clear_screen()
            print(f"{BOLD}{CYAN}=================================================={RESET}")
            print(f"{BOLD}{WHITE} Metroid Zero Mission 3DS - Asistente de Build{RESET}")
            print(f"{BOLD}{CYAN}=================================================={RESET}\n")

            print(f"{BOLD}{title}{RESET}")
            if subtitle:
                print(f"{DIM}{subtitle}{RESET}")
            print()

            for i, opt in enumerate(options):
                if i == current:
                    label, desc = opt if isinstance(opt, tuple) else (opt, "")
                    print(f"  {BOLD}{GREEN}➔ [X] {label}{RESET}")
                    if desc:
                        print(f"        {DIM}{desc}{RESET}")
                else:
                    label, desc = opt if isinstance(opt, tuple) else (opt, "")
                    print(f"    [ ] {label}")
                    if desc:
                        print(f"        {DIM}{desc}{RESET}")

            print(f"\n{DIM}(Usa las flechas ↑ / ↓ o W/S para moverte, Enter para elegir, Q para salir){RESET}")

            key = term.get_key()
            if key == "UP":
                current = (current - 1) % num_options
            elif key == "DOWN":
                current = (current + 1) % num_options
            elif key == "ENTER" or key == "SPACE":
                return current
            elif key in ("ESC", "QUIT"):
                print(f"\n{YELLOW}Operación cancelada.{RESET}")
                sys.exit(0)


def prompt_text(prompt_label, default_value=""):
    """Solicita una entrada de texto interactiva."""
    clear_screen()
    print(f"{BOLD}{CYAN}=================================================={RESET}")
    print(f"{BOLD}{WHITE} Metroid Zero Mission 3DS - Asistente de Build{RESET}")
    print(f"{BOLD}{CYAN}=================================================={RESET}\n")

    display_prompt = f"{BOLD}{prompt_label}{RESET}"
    if default_value:
        display_prompt += f" [{CYAN}{default_value}{RESET}]: "
    else:
        display_prompt += ": "

    try:
        val = input(display_prompt).strip()
        if not val and default_value:
            return default_value
        return val
    except (KeyboardInterrupt, EOFError):
        print(f"\n{YELLOW}Operación cancelada.{RESET}")
        sys.exit(0)


def load_last_ip():
    """Carga la última IP utilizada si existe el archivo."""
    if os.path.isfile(IP_HISTORY_FILE):
        try:
            with open(IP_HISTORY_FILE, "r", encoding="utf-8") as f:
                ip = f.read().strip()
                if ip:
                    return ip
        except Exception:
            pass
    return "192.168.1."


def save_last_ip(ip):
    """Guarda la IP utilizada."""
    try:
        with open(IP_HISTORY_FILE, "w", encoding="utf-8") as f:
            f.write(ip.strip())
    except Exception:
        pass


def find_make():
    """Localiza el ejecutable de make en el sistema o dentro de devkitPro."""
    make_bin = shutil.which("make")
    if make_bin:
        return make_bin

    devkitpro = os.environ.get("DEVKITPRO", "/opt/devkitpro" if os.name != "nt" else r"C:\devkitPro")
    candidates = [
        os.path.join(devkitpro, "msys2", "usr", "bin", "make.exe"),
        os.path.join(devkitpro, "devkitARM", "bin", "make.exe"),
        os.path.join(devkitpro, "tools", "bin", "make.exe"),
    ]
    for c in candidates:
        if os.path.isfile(c):
            return c
    return "make"


def extract_errors(output_text):
    """Extrae las líneas relevantes de error de la salida de make."""
    error_lines = []
    for line in output_text.splitlines():
        line_clean = line.strip()
        if not line_clean:
            continue
        # Descartar fragmentos de código fuente que GCC imprime con número de línea (ej. "70 | ...")
        if re.match(r"^\d+\s*\|", line_clean):
            continue
        # Descartar warnings que no contengan error
        if "warning:" in line.lower() and "error:" not in line.lower():
            continue
        if any(marker in line.lower() for marker in ("error:", "error 127", "error 1", "error 2", "not found", "no such file", "undefined reference", "fatal error:")):
            if "espera a que terminen otras tareas" not in line.lower() and "sale del directorio" not in line.lower():
                if line_clean not in error_lines:
                    error_lines.append(line_clean)
    return error_lines


def run_build(mode, send_ftp=False, ftp_host="", ftp_port=5000, clean=True, dry_run=False, jobs=None, verbose=False):
    """Ejecuta la compilación según los parámetros indicados."""
    if jobs is None:
        jobs = os.cpu_count() or 4

    debug_tools = 1 if mode in ("test", "debug") else 0
    mode_name = "Debug / Test (DEBUG_TOOLS=1)" if debug_tools == 1 else "Producción (DEBUG_TOOLS=0)"

    print(f"\n{BOLD}{CYAN}=================================================={RESET}")
    print(f"{BOLD}{WHITE} Resumen de Compilación 3DS{RESET}")
    print(f"{BOLD}{CYAN}=================================================={RESET}")
    print(f"  {BOLD}Modo:{RESET}       {GREEN if debug_tools == 1 else MAGENTA}{mode_name}{RESET}")
    print(f"  {BOLD}Limpiar:{RESET}    {'Sí (make clean)' if clean else 'No (incremental)'}")
    if send_ftp:
        print(f"  {BOLD}Envío FTP:{RESET}  {YELLOW}Sí ➔ {ftp_host}:{ftp_port}{RESET}")
    else:
        print(f"  {BOLD}Envío FTP:{RESET}  No (solo .cia local)")
    print(f"  {BOLD}Hilos (jobs):{RESET} {jobs}")
    if verbose:
        print(f"  {BOLD}Salida:{RESET}     Detallada (verbose)")
    else:
        print(f"  {BOLD}Salida:{RESET}     Limpia (usa -v para salida detallada)")
    print(f"{BOLD}{CYAN}=================================================={RESET}\n")

    make_bin = find_make()
    env = os.environ.copy()
    
    # Asegurar rutas a herramientas (bannertool, makerom, etc.)
    tool_paths = [
        os.path.join(ROOT, "tools", "bin"),
        os.path.join(os.path.expanduser("~"), "tools", "bin"),
        os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "tools", "bin"),
        os.path.join(os.environ.get("DEVKITPRO", "/opt/devkitpro"), "devkitARM", "bin"),
    ]
    extra_path = os.pathsep.join([p for p in tool_paths if os.path.isdir(p)])
    if extra_path:
        env["PATH"] = extra_path + os.pathsep + env.get("PATH", "")

    if send_ftp:
        env["FTP_HOST"] = ftp_host
        env["FTP_PORT"] = str(ftp_port)

    # Paso 1: Limpieza si se solicita
    if clean:
        clean_cmd = [make_bin, "-C", PLATFORM_3DS, "clean"]
        print(f"{BOLD}{CYAN}[1/2]{RESET} Limpiando objetos anteriores (make clean)...")
        if dry_run:
            print(f"      {DIM}{' '.join(clean_cmd)}{RESET}")
        else:
            if verbose:
                ret = subprocess.run(clean_cmd, env=env)
                if ret.returncode != 0:
                    print(f"\n{RED}Error durante 'make clean'.{RESET}")
                    return ret.returncode
            else:
                proc = subprocess.run(clean_cmd, env=env, capture_output=True, text=True)
                if proc.returncode != 0:
                    print(f"\n{BOLD}{RED}✗ Error durante 'make clean':{RESET}")
                    errs = extract_errors(proc.stderr + "\n" + proc.stdout)
                    for err in errs or [proc.stderr.strip()]:
                        print(f"  {RED}• {err}{RESET}")
                    return proc.returncode

    # Paso 2: Compilación (y envío si aplica)
    target = "ftp" if send_ftp else "cia"
    build_cmd = [
        make_bin,
        "-C",
        PLATFORM_3DS,
        f"-j{jobs}",
        f"DEBUG_TOOLS={debug_tools}",
        target
    ]

    action_label = f"Compilando y enviando por FTP a {ftp_host}:{ftp_port}" if send_ftp else "Compilando binario 3DS y empaquetando CIA"
    step_num = "[2/2]" if clean else "[1/1]"
    print(f"{BOLD}{CYAN}{step_num}{RESET} {action_label}...")

    if dry_run:
        print(f"      {DIM}{' '.join(build_cmd)}{RESET}")
        print(f"\n{GREEN}[Dry-Run] Simulación completada correctamente.{RESET}")
        return 0

    if verbose:
        ret = subprocess.run(build_cmd, env=env)
        returncode = ret.returncode
        full_output = ""
    else:
        proc = subprocess.run(build_cmd, env=env, capture_output=True, text=True)
        returncode = proc.returncode
        full_output = proc.stdout + "\n" + proc.stderr

    if returncode == 0:
        if send_ftp:
            print(f"\n{BOLD}{GREEN}✓ Compilación y subida por FTP a {ftp_host}:{ftp_port} completadas con éxito!{RESET}")
        else:
            cia_path = os.path.join(PLATFORM_3DS, "mzm-3ds.cia")
            print(f"\n{BOLD}{GREEN}✓ Compilación completada con éxito!{RESET}")
            if os.path.isfile(cia_path):
                size_mb = os.path.getsize(cia_path) / (1024 * 1024)
                print(f"  Archivo CIA generado: {BOLD}{cia_path}{RESET} ({size_mb:.2f} MB)")
    else:
        print(f"\n{BOLD}{RED}✗ Error durante la compilación:{RESET}")
        if not verbose and full_output:
            errs = extract_errors(full_output)
            if errs:
                print(f"{DIM}--------------------------------------------------{RESET}")
                for err in errs:
                    print(f"  {RED}• {err}{RESET}")
                print(f"{DIM}--------------------------------------------------{RESET}")
            else:
                # Si no se filtró nada específico, mostrar últimas líneas de stderr
                last_lines = [l for l in full_output.strip().splitlines() if l.strip()][-6:]
                print(f"{DIM}--------------------------------------------------{RESET}")
                for l in last_lines:
                    print(f"  {RED}{l}{RESET}")
                print(f"{DIM}--------------------------------------------------{RESET}")
            print(f"{DIM}(Ejecuta con -v o --verbose si deseas ver la salida completa del compilador){RESET}")

    return returncode


def parse_ftp_argument(ftp_str, explicit_ip=None, default_port=5000):
    """Extrae host y puerto de strings tipo '192.168.1.50' o '192.168.1.50:5000'."""
    raw = explicit_ip or ftp_str or ""
    raw = raw.strip()
    if not raw or raw.lower() == "true":
        return "", default_port

    if ":" in raw:
        parts = raw.split(":", 1)
        host = parts[0].strip()
        try:
            port = int(parts[1].strip())
        except ValueError:
            port = default_port
        return host, port
    return raw, default_port


def main():
    parser = argparse.ArgumentParser(
        description="Compila Metroid Zero Mission 3DS en modo Debug o Producción, con opción de envío FTP.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Ejemplos de uso:
  # Menú interactivo:
  python3 tools/build_3ds.py

  # Compilar en modo debug/test:
  python3 tools/build_3ds.py --mode test

  # Compilar en modo producción:
  python3 tools/build_3ds.py --mode prod

  # Compilar y enviar por FTP (modo debug por defecto):
  python3 tools/build_3ds.py --ftp 192.168.1.55

  # Compilar en producción y enviar por FTP a puerto específico:
  python3 tools/build_3ds.py --mode prod --ftp 192.168.1.55:5000
"""
    )
    parser.add_argument(
        "-m", "--mode",
        choices=["test", "debug", "prod", "production", "release"],
        help="Modo de compilación: 'test'/'debug' (DEBUG_TOOLS=1) o 'prod'/'production'/'release' (DEBUG_TOOLS=0)."
    )
    parser.add_argument(
        "-f", "--ftp",
        nargs="?",
        const="true",
        default=None,
        metavar="IP[:PUERTO]",
        help="Habilita envío por FTP a la 3DS. Puede especificarse la IP (ej. 192.168.1.50 o 192.168.1.50:5000)."
    )
    parser.add_argument(
        "--ip",
        metavar="IP",
        help="Dirección IP de la Nintendo 3DS para el envío FTP."
    )
    parser.add_argument(
        "-p", "--port",
        type=int,
        default=5000,
        help="Puerto FTP de la Nintendo 3DS (por defecto: 5000)."
    )
    parser.add_argument(
        "-c", "--clean",
        action="store_true",
        default=None,
        help="Forzar 'make clean' antes de compilar."
    )
    parser.add_argument(
        "--no-clean",
        action="store_true",
        help="Omitir 'make clean' (compilación incremental)."
    )
    parser.add_argument(
        "-j", "--jobs",
        type=int,
        default=None,
        help="Número de procesos paralelos para make."
    )
    parser.add_argument(
        "-i", "--interactive",
        action="store_true",
        help="Forzar menú interactivo incluso con parámetros."
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Muestra toda la salida detallada del compilador."
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Muestra las órdenes a ejecutar sin realizarlas."
    )

    args = parser.parse_args()

    # Si se pide interactivo o no se pasan argumentos y es una terminal TTY:
    is_interactive = args.interactive or (
        args.mode is None and args.ftp is None and args.ip is None and sys.stdin.isatty()
    )

    if is_interactive:
        # Menú 1: Modo de compilación
        mode_options = [
            ("Modo Debug / Test (Recomendado para pruebas)",
             "Activa menú DEBUG inferior, herramientas de testeo y diagnósticos (DEBUG_TOOLS=1)."),
            ("Modo Producción / Release",
             "Binario limpio y optimizado para jugar (DEBUG_TOOLS=0).")
        ]
        mode_idx = interactive_select(
            "Selecciona el modo de compilación:",
            mode_options,
            default_index=0
        )
        selected_mode = "test" if mode_idx == 0 else "prod"

        # Menú 2: Enviar por FTP
        ftp_options = [
            ("No, solo compilar el archivo .cia localmente",
             "El archivo .cia quedará en platform/3ds/mzm-3ds.cia"),
            ("Sí, compilar y enviar automáticamente por FTP a la 3DS",
             "Requiere tener la 3DS encendida con FBI o FTPD en la misma red Wi-Fi.")
        ]
        # Si estamos en modo debug o test, preseleccionar según preferencia habitual
        ftp_idx = interactive_select(
            "¿Deseas enviar el CIA por FTP a tu Nintendo 3DS?",
            ftp_options,
            default_index=0
        )
        send_ftp = (ftp_idx == 1)

        ftp_host = ""
        ftp_port = args.port or 5000

        if send_ftp:
            last_ip = load_last_ip()
            while not ftp_host:
                ip_input = prompt_text(
                    "Introduce la dirección IP de la Nintendo 3DS",
                    default_value=last_ip
                )
                host, port = parse_ftp_argument(ip_input, default_port=ftp_port)
                if host:
                    ftp_host = host
                    ftp_port = port
                    save_last_ip(host)
                else:
                    print(f"{RED}Error: Debes introducir una dirección IP válida.{RESET}")

        # Menú 3: Clean build
        clean_options = [
            ("Sí (Limpiar antes de compilar - Recomendado)",
             "Ejecuta 'make clean' para asegurar que no haya objetos (.o) desactualizados."),
            ("No (Compilación incremental)",
             "Más rápido si solo has modificado pocos archivos.")
        ]
        clean_idx = interactive_select(
            "¿Deseas realizar una compilación limpia (make clean)?",
            clean_options,
            default_index=0
        )
        do_clean = (clean_idx == 0)

        return run_build(
            mode=selected_mode,
            send_ftp=send_ftp,
            ftp_host=ftp_host,
            ftp_port=ftp_port,
            clean=do_clean,
            dry_run=args.dry_run,
            jobs=args.jobs,
            verbose=args.verbose
        )

    # Modo No Interactivo (CLI)
    send_ftp = (args.ftp is not None) or (args.ip is not None)
    ftp_host, ftp_port = parse_ftp_argument(args.ftp, explicit_ip=args.ip, default_port=args.port)

    # Regla: Por FTP, por defecto, a no ser que se diga lo contrario, siempre será en modo debug.
    if args.mode is None:
        if send_ftp:
            mode = "test"  # Default para FTP
        else:
            mode = "test"  # Default general
    else:
        mode = "test" if args.mode in ("test", "debug") else "prod"

    if send_ftp and not ftp_host:
        # Si se especificó --ftp sin IP, intentar usar la guardada o pedirla
        ftp_host = load_last_ip()
        if not ftp_host or ftp_host == "192.168.1.":
            print(f"{RED}Error: Se ha solicitado envío por FTP pero no se especificó la IP.{RESET}")
            print("Usa: --ftp 192.168.1.xxx o --ip 192.168.1.xxx")
            sys.exit(1)

    if send_ftp and ftp_host:
        save_last_ip(ftp_host)

    do_clean = True
    if args.no_clean:
        do_clean = False
    elif args.clean is not None:
        do_clean = args.clean

    return run_build(
        mode=mode,
        send_ftp=send_ftp,
        ftp_host=ftp_host,
        ftp_port=ftp_port,
        clean=do_clean,
        dry_run=args.dry_run,
        jobs=args.jobs,
        verbose=args.verbose
    )


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print(f"\n{YELLOW}Interrumpido por el usuario.{RESET}")
        sys.exit(130)
