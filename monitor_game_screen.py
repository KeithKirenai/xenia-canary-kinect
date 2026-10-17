import time
import os
import subprocess
import win32gui
import win32ui
import win32con
from PIL import Image
import winocr

RUNTIME_SECONDS = 130 # ~2 minutes + startup
SCREENSHOT_INTERVAL = 10 # Capture every 10 seconds
SNAPSHOTS_DIR = os.path.abspath("test_snapshots")
ISO_PATH = r"C:\Users\Carlos\Downloads\Kinect Adventures! (USA) (En,Ja,Fr,De,Es,It,Nl,Pt,Zh,Ko,Pl,Ru) (Rev 2).iso"
XENIA_EXE = r"build\bin\Windows\Release\xenia_canary.exe"

os.makedirs(SNAPSHOTS_DIR, exist_ok=True)

def find_xenia_hwnd():
    found_hwnd = None
    def enum_cb(hwnd, extra):
        nonlocal found_hwnd
        if win32gui.IsWindowVisible(hwnd):
            title = win32gui.GetWindowText(hwnd)
            if "Xenia" in title or "Kinect Adventures" in title:
                found_hwnd = hwnd
    win32gui.EnumWindows(enum_cb, None)
    return found_hwnd

def capture_window(hwnd, output_path):
    try:
        # Get client rect
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
        
        # PrintWindow with PW_CLIENTONLY (flag 1 or 2)
        import ctypes
        ctypes.windll.user32.PrintWindow(hwnd, save_dc.GetSafeHdc(), 2)
        
        bmpinfo = save_bitmap.GetInfo()
        bmpstr = save_bitmap.GetBitmapBits(True)
        img = Image.frombuffer('RGB', (bmpinfo['bmWidth'], bmpinfo['bmHeight']), bmpstr, 'raw', 'BGRX', 0, 1)
        
        win32gui.DeleteObject(save_bitmap.GetHandle())
        save_dc.DeleteDC()
        mfc_dc.DeleteDC()
        win32gui.ReleaseDC(hwnd, hwnd_dc)
        
        img.save(output_path)
        return img
    except Exception as e:
        print(f"Capture error: {e}")
        return None

def run_ocr(img):
    try:
        op = winocr.recognize_pil(img, 'en')
        res = op.get()
        return res.text.strip()
    except Exception as e:
        return f"[OCR Error: {e}]"

def main():
    print(f"=== INICIANDO PRUEBA EXTENDIDA DE 2 MINUTOS CON OCR NATIVO ===")
    print("1. Cerrando instancias previas...")
    subprocess.run(["powershell", "-Command", "Stop-Process -Name 'xenia_canary' -Force -ErrorAction SilentlyContinue"], capture_output=True)
    time.sleep(1)
    
    print("2. Lanzando Xenia Canary...")
    proc = subprocess.Popen([XENIA_EXE, ISO_PATH])
    
    start_time = time.time()
    next_snap = start_time + 10
    snap_count = 0
    records = []
    
    print("3. Monitoreando ventana y capturando texto...")
    while time.time() - start_time < RUNTIME_SECONDS:
        now = time.time()
        if now >= next_snap:
            elapsed = int(now - start_time)
            hwnd = find_xenia_hwnd()
            if hwnd:
                snap_count += 1
                img_path = os.path.join(SNAPSHOTS_DIR, f"snap_{elapsed:03d}s.png")
                img = capture_window(hwnd, img_path)
                if img:
                    text = run_ocr(img)
                    text_clean = " ".join(text.split())
                    print(f"[{elapsed:03d}s] OCR: '{text_clean}'")
                    records.append((elapsed, text_clean))
                else:
                    print(f"[{elapsed:03d}s] Fallo al capturar ventana.")
            else:
                print(f"[{elapsed:03d}s] Ventana de Xenia aun no encontrada.")
            next_snap = now + SCREENSHOT_INTERVAL
        time.sleep(0.5)
        
        if proc.poll() is not None:
            print("[ALERTA] Xenia se cerro prematuramente!")
            break
            
    print("4. Cerrando emulador tras completar el tiempo...")
    if proc.poll() is None:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except:
            proc.kill()
            
    print("\n=== RESUMEN CRONOLÓGICO DE TEXTO EN PANTALLA (OCR) ===")
    for el, txt in records:
        print(f"T+{el:03d}s : {txt}")

if __name__ == "__main__":
    main()
