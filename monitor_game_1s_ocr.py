import subprocess
import time
import os
import sys
import argparse
import threading
import hashlib
import win32gui
import win32ui
import ctypes
from PIL import Image, ImageOps, ImageEnhance
import winocr

XENIA_EXE = r"build\bin\Windows\Release\xenia_canary.exe"
GAME_ISO = r"C:\Users\Carlos\Downloads\Kinect Adventures! (USA) (En,Ja,Fr,De,Es,It,Nl,Pt,Zh,Ko,Pl,Ru) (Rev 2).iso"
DURATION_SECONDS = 75
SAMPLE_INTERVAL_SECONDS = 3
LOG_OUTPUT_FILE = "per_second_ocr_log.txt"

# Set by the watchdog thread when Xenia pops its crash dialog.
CRASH_EVENT = threading.Event()

def frame_signature(img):
    """Cheap perceptual hash of a screenshot, used to detect a frozen picture.

    A frozen frame and a running-but-idle frame are hard to tell apart from OCR
    text alone (the 'IS ANYBODY THERE?' screen legitimately repeats). Hashing
    the pixels separates them: these screens animate, so N byte-identical
    captures in a row means the emulator stopped presenting frames.
    """
    if img is None:
        return None
    try:
        small = img.convert('L').resize((64, 36))
        return hashlib.md5(small.tobytes()).hexdigest()
    except Exception:
        return None

def find_xenia_hwnd():
    """Return the HWND of the emulator's own top-level window.

    Match on the owning *process* (xenia_canary.exe) rather than the title
    text: a title substring match also hits terminals/IDEs whose title merely
    contains "Xenia" (e.g. the repo path), which made the OCR capture the
    console instead of the game.
    """
    import win32process

    target_pids = set()
    try:
        import psutil
        for p in psutil.process_iter(["pid", "name"]):
            if (p.info["name"] or "").lower() in ("xenia_canary.exe", "xenia.exe"):
                target_pids.add(p.info["pid"])
    except Exception:
        # Fall back to matching the window class/owner via the process name.
        pass

    found = []

    def enum_cb(hwnd, extra):
        if not win32gui.IsWindowVisible(hwnd):
            return
        try:
            _, pid = win32process.GetWindowThreadProcessId(hwnd)
        except Exception:
            return
        if target_pids and pid not in target_pids:
            return
        rect = win32gui.GetClientRect(hwnd)
        width = rect[2] - rect[0]
        height = rect[3] - rect[1]
        if width < 300 or height < 200:
            return
        title = win32gui.GetWindowText(hwnd)
        if "Xenia" in title or "Kinect Adventures" in title or not title:
            found.append(hwnd)

    try:
        win32gui.EnumWindows(enum_cb, None)
    except Exception:
        pass
    return found[0] if found else None

def find_crash_dialog():
    """Return hwnd of an 'Unhandled Exception in Xenia' MessageBox, if shown.

    Xenia's crash handler uses a blocking MessageBoxA, so the emulator process
    stays alive but frozen forever. Detect it so the run can be torn down
    instead of waiting for the full duration.
    """
    found = []
    def enum_cb(hwnd, extra):
        if not win32gui.IsWindowVisible(hwnd):
            return
        title = win32gui.GetWindowText(hwnd)
        cls = win32gui.GetClassName(hwnd)
        if cls == "#32770" and "Unhandled Exception" in title:
            found.append(hwnd)
    try:
        win32gui.EnumWindows(enum_cb, None)
    except Exception:
        pass
    return found[0] if found else None

def log_crash_context(tail_lines=25):
    """Print the exception block from the tail of the xenia log."""
    log_path = os.path.join("build", "bin", "Windows", "Release", "xenia.log")
    if not os.path.exists(log_path):
        print("[CRASH] No se encontro xenia.log", flush=True)
        return
    try:
        size = os.path.getsize(log_path)
        with open(log_path, "rb") as f:
            start = max(0, size - 200000)
            f.seek(start)
            text = f.read().decode("utf-8", errors="replace")
        lines = text.splitlines()
        crash_at = None
        for i, ln in enumerate(lines):
            if "CRITICAL: Unhandled Exception" in ln:
                crash_at = i
                break
        print("\n[CRASH] Xenia lanzo una excepcion no controlada:", flush=True)
        if crash_at is None:
            print("        (bloque no encontrado en el log; ultimas lineas:)", flush=True)
            for ln in lines[-tail_lines:]:
                print(f"        {ln}", flush=True)
        else:
            lo = max(0, crash_at - 12)
            for ln in lines[lo:crash_at + 14]:
                print(f"        {ln}", flush=True)
    except Exception as e:
        print(f"[CRASH] No se pudo leer el log: {e}", flush=True)

def capture_window_viewport(hwnd):
    try:
        cl_left, cl_top, cl_right, cl_bottom = win32gui.GetClientRect(hwnd)
        width = cl_right - cl_left
        height = cl_bottom - cl_top
        if width <= 0 or height <= 0:
            return None
        
        hwnd_dc = win32gui.GetDC(hwnd)
        mfc_dc = win32ui.CreateDCFromHandle(hwnd_dc)
        save_dc = mfc_dc.CreateCompatibleDC()
        save_bitmap = win32ui.CreateBitmap()
        save_bitmap.CreateCompatibleBitmap(mfc_dc, width, height)
        save_dc.SelectObject(save_bitmap)
        
        ctypes.windll.user32.PrintWindow(hwnd, save_dc.GetSafeHdc(), 2)
        
        bmpinfo = save_bitmap.GetInfo()
        bmpstr = save_bitmap.GetBitmapBits(True)
        img = Image.frombuffer('RGB', (bmpinfo['bmWidth'], bmpinfo['bmHeight']), bmpstr, 'raw', 'BGRX', 0, 1)
        
        win32gui.DeleteObject(save_bitmap.GetHandle())
        save_dc.DeleteDC()
        mfc_dc.DeleteDC()
        win32gui.ReleaseDC(hwnd, hwnd_dc)
        
        return img
    except Exception:
        return None

def start_crash_watchdog(proc):
    """Background thread that dismisses Xenia's modal crash dialog.

    Xenia's crash handler calls MessageBoxA, which blocks the emulator thread
    and leaves the process alive forever. Polling only once per OCR sample (3s)
    was too slow in practice, so watch for the dialog on a tight 0.25s loop,
    log the exception, then tear the process down.
    """
    def _watch():
        while not CRASH_EVENT.is_set():
            if proc.poll() is not None:
                # Process already gone; nothing to dismiss.
                return
            hwnd = find_crash_dialog()
            if hwnd:
                CRASH_EVENT.set()
                print("\n[watchdog] Dialogo de excepcion detectado -> cerrando Xenia", flush=True)
                # Give the handler a moment to finish writing the dump/log.
                time.sleep(1.0)
                log_crash_context()
                try:
                    win32gui.PostMessage(hwnd, win32gui.WM_CLOSE, 0, 0)
                except Exception:
                    pass
                time.sleep(0.3)
                try:
                    proc.kill()
                except Exception:
                    pass
                return
            time.sleep(0.25)

    t = threading.Thread(target=_watch, daemon=True)
    t.start()
    return t

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--seconds", type=float, default=DURATION_SECONDS)
    ap.add_argument("--interval", type=float, default=SAMPLE_INTERVAL_SECONDS)
    # Stop as soon as this many consecutive captures are pixel-identical.
    ap.add_argument("--freeze-exit", type=int, default=4)
    # Ignore freeze detection during the first N seconds (splash/loading is static).
    ap.add_argument("--warmup", type=float, default=10.0)
    # Stop early if any of these substrings appear in the OCR text.
    ap.add_argument("--stop-on-text", nargs="*", default=None)
    # Extra Xenia config variables, repeatable: --cvar name=value
    # Passed on the command line because Xenia rewrites its .toml on exit and
    # would otherwise persist the compiled-in default over any manual edit.
    ap.add_argument("--cvar", action="append", default=[])
    args = ap.parse_args()

    xenia_cmd = [XENIA_EXE]
    for c in args.cvar:
        if "=" not in c:
            print(f"[ERROR] --cvar necesita nombre=valor, recibido: {c}", flush=True)
            sys.exit(2)
        xenia_cmd.append(f"--{c}")
    xenia_cmd.append(GAME_ISO)
    if args.cvar:
        print(f"Config vars: {', '.join(args.cvar)}", flush=True)
    duration = args.seconds
    interval = args.interval

    print(f"=== MONITORIZACION VISUAL OCR CADA {interval} SEGUNDOS ({duration}s MAX) ===", flush=True)
    subprocess.run(["powershell", "-Command", "Stop-Process -Name 'xenia_canary','xenia' -Force -ErrorAction SilentlyContinue"], capture_output=True)
    time.sleep(1)

    print("Lanzando Xenia Canary con Kinect Adventures...", flush=True)
    proc = subprocess.Popen(xenia_cmd)

    # Esperar a que la ventana aparezca
    hwnd = None
    print("Esperando deteccion de ventana...", flush=True)
    for _ in range(40):
        time.sleep(0.5)
        hwnd = find_xenia_hwnd()
        if hwnd:
            break

    if not hwnd:
        print("[ERROR] No se pudo encontrar la ventana de Xenia.", flush=True)
        if proc.poll() is None:
            proc.kill()
        return

    print(f"Ventana detectada (HWND: {hwnd}). Iniciando lectura OCR cada {interval}s durante {duration}s...\n", flush=True)

    # Watch for the modal crash dialog on a tight loop so the emulator always
    # tears itself down instead of hanging until someone clicks the dialog.
    CRASH_EVENT.clear()
    start_crash_watchdog(proc)

    timeline = []
    start_time = time.time()
    crashed = False
    exit_reason = "dURACION"

    # Open the report up front and write every sample immediately. If the run is
    # killed from the outside (or the harness is interrupted) the partial
    # timeline still survives on disk instead of being lost with the process.
    log_f = open(LOG_OUTPUT_FILE, "w", encoding="utf-8")
    log_f.write(f"# inicio {time.strftime('%H:%M:%S')} duracion_max={duration}s "
                f"intervalo={interval}s freeze_exit={args.freeze_exit} "
                f"stop_on_text={args.stop_on_text}\n")

    last_sig = None
    same_count = 0
    elapsed = 0
    while elapsed <= duration:
        # Watchdog thread flags the modal crash dialog; check it here so the capture
        # loop stops promptly instead of waiting out the full duration.
        if CRASH_EVENT.is_set():
            print(f"\n[T+{int(elapsed):03d}s] *** EXCEPCION NO CONTROLADA DETECTADA ***", flush=True)
            crashed = True
            exit_reason = "EXCEPCION"
            break
        if proc.poll() is not None:
            # Clean exit (no crash dialog was seen) -- e.g. the window was
            # closed manually. Report it distinctly from a crash.
            print(f"\n[T+{int(elapsed):03d}s] Xenia se cerró (codigo {proc.returncode}).", flush=True)
            exit_reason = f"PROCESO_SALIO(code={proc.returncode})"
            break

        second = int(elapsed)
        img = capture_window_viewport(hwnd)

        # Early exit: freeze detection via pixel hash, and target-text hits.
        sig = frame_signature(img)
        if elapsed >= args.warmup and sig is not None and args.freeze_exit > 0:
            if sig == last_sig:
                same_count += 1
            else:
                same_count = 0
            last_sig = sig
            if same_count >= args.freeze_exit:
                print(f"\n[T+{int(elapsed):03d}s] PANTALLA CONGELADA "
                      f"({same_count} capturas identicas) -> corte temprano", flush=True)
                exit_reason = "PANTALLA_CONGELADA"
                break

        text = ""
        if img:
            try:
                w, h = img.size
                if h > 80:
                    game_viewport = img.crop((0, 60, w, h))
                else:
                    game_viewport = img
                
                passes = [
                    game_viewport,
                    game_viewport.convert('L'),
                    ImageOps.invert(game_viewport.convert('L')),
                    ImageOps.autocontrast(game_viewport.convert('L')),
                    ImageEnhance.Contrast(game_viewport.convert('L')).enhance(2.5),
                ]
                detected_phrases = []
                for p_img in passes:
                    try:
                        res = winocr.recognize_pil_sync(p_img)
                        cand = res.get('text', '').strip()
                        if cand and cand not in detected_phrases:
                            detected_phrases.append(cand)
                    except Exception:
                        pass
                
                text = " | ".join(detected_phrases) if detected_phrases else ""
            except Exception as e:
                text = f"[OCR Error: {e}]"
        else:
            text = "[FRAME_NOT_READY]"

        if not text:
            text = "[SIN TEXTO DETECTADO]"

        log_line = f"T+{second:03d}s : {text}"
        print(log_line, flush=True)
        timeline.append(log_line)
        log_f.write(log_line + "\n")
        log_f.flush()

        if args.stop_on_text:
            upper = text.upper()
            for needle in args.stop_on_text:
                if needle.upper() in upper:
                    print(f"\n[T+{second:03d}s] Texto objetivo '{needle}' alcanzado -> corte temprano", flush=True)
                    exit_reason = f"OBJETIVO_ALCANZADO({needle})"
                    elapsed = duration  # break out on the next guard
                    break

        time.sleep(interval)
        elapsed = time.time() - start_time

    if crashed:
        print("\nCerrando emulador tras la excepcion...", flush=True)
    else:
        print(f"\nCorte por: {exit_reason}. Cerrando emulador...", flush=True)

    if crashed:
        # The watchdog thread already printed the exception block; just make
        # sure the process and any lingering dialog are gone.
        h = find_crash_dialog()
        if h:
            try:
                win32gui.PostMessage(h, win32gui.WM_CLOSE, 0, 0)
            except Exception:
                pass
    subprocess.run(["powershell", "-Command", "Stop-Process -Name 'xenia_canary','xenia' -Force -ErrorAction SilentlyContinue"], capture_output=True)

    log_f.write(f"# fin motivo={exit_reason} capturado={len(timeline)} muestras\n")
    log_f.close()

    if crashed:
        print(f"\n*** PRUEBA TERMINADA POR EXCEPCION. Timeline guardado en: {LOG_OUTPUT_FILE} ***", flush=True)
    else:
        print(f"\nReporte cronologico guardado exitosamente en: {LOG_OUTPUT_FILE} ({len(timeline)} muestras)", flush=True)

if __name__ == "__main__":
    main()
