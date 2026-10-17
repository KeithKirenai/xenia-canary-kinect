# Kinect Adventures — Progress Log

## Sesión 2026-10-16 (c): la tubería NUI es push-driven y su scheduler nunca se registró

### El título NO pide esqueletos: los empujan

`XamUserNuiGetUserIndex` = 0 llamadas, `XamXStudioRequest` = 0 llamadas. La
razón es de arquitectura, no un bug de stubs.

**Worker del título** (`0x82C48D18`, NUIPRE embebido, TID 9):

```asm
loop:
  li   r6, 0                 ; Timeout = 0  -> NO bloquea, es un poll
  li   r4, 3                 ; WaitMode
  bl   KeWaitForSingleObject(worker+0x3c, 3, 1, 0)
  cmplwi cr6, r3, 0x102      ; STATUS_TIMEOUT?
  beq   -> loop
  bl   KeResetEvent
  lwz  r11, 4(r25)           ; hay trabajo pendiente?
  beq   -> return            ; no -> el hilo termina
  bl   0x82c48a58            ; ProcessQueue
  b    loop
```

**Productor** (`0x82c48eb0`): postea un item de 32 bits en un anillo de 4 y
luego `KeSetEvent`. Nunca hay item -> el worker despierta y no hace nada.

### Layout real (obtenido del heap en runtime)

| objeto | dirección |
|---|---|
| worker TID 9 | `0x4004E998` |
| worker TID A | `0x4004EA04` |
| scheduler (`r25` / `r31`) | `0x4004E744` |
| registro product/consumidor | `0x4004E9C4` = `{0x82C48EB0, worker}` |

Worker: `+0x2C` = `0x82C48EB0` (callback de push), `+0x30` = self,
`+0x48` = handle del evento (`F800002C` / `F8000030`), `+0x54`/`+0x58` =
head/tail del anillo, items en `+0x5C + (idx & 3)*4`.

**Estado observado: `head == tail == 0`, los 4 slots en cero.**

### Causa raíz

El scheduler en `0x4004E744` está **completamente vacío**:

```
+0x10 = 0   <- handler que invoca mtctr/bctrl
+0x14 = 0
+0x164 = 0 / +0x168 = 0
+0x18 = 0 / +0x1C = 0   <- rango de iteración vacío
```

Nunca se registró ningún procesador de stream, así que `r31[0x10]` es null y
el worker nunca tiene nada que despachar.

### Formato del work item: es bytecode

`ProcessQueue` recibe `p` y trata `p+4` como una cadena de palabras:

| palabra | efecto |
|---|---|
| `0x8C00` + u32 | instala la función de dispatch en `r31[0x10]` |
| `0x8D00` + u32 | instala en `r31[0x168]` |
| bit0 = 1 | **invoca** `r31[0x10](worker, payload)` |

Un item **no se autoinstala**: necesita punteros a funciones reales del título.

### El título NUNCA construye esos items

Barrido de `.text` completo: **0 ocurrencias** de `0x8C00`/`0x8D00` como
inmediatos (`lis`/`ori`/`addi`). Solo `ProcessQueue` los *consume*.

Por lo tanto el productor es **XAM**, no el título. Nosotros somos XAM.

> Nota: `worker+0x44 = 0x58454E00` **no** es un magic de clase ("NEX"); es un
> constante más del mismo campo de bitmasks (`0xC0000102`, `0x7F000100`,
> `0x00130003`). No se pursued esa pista.

### `XamXStudioRequest(0x1502)` es un PULL

En `0x82D35720`: `XamXStudioRequest(0x1502, struct@r1+0x70)` donde el struct
lleva un buffer de salida de `0x68` (104) bytes en `r1+0x1B0`. El título
*descarga* frames; un work item es lo que dispara esa descarga.

### Próximo paso

Dumpear el código del módulo `xam.xex` (el dump de secciones existente **no
produjo ningún archivo**) y localizar allí la rutina NUI que construye el item
`0x8C00` + handler. Ahí está el productor que falta.

---

## Sesión 2026-10-16 (b): contrato real de 0x2B001, harness eliminado

### 0x2B001 lleva UN solo dword (no 8 argumentos)

Verificado en el caller del guest, `0x831AEF60`:

```asm
stw  r3, 0x50(r1)          ; el único argumento
addi r5, r1, 0x50
li   r6, 4                 ; buffer_length = 4
li   r3, 0xfe
lis  r4, 2 / ori r4,0xb001
bl   XMsgSystemProcessCall(0xFE, 0x2B001, r1+0x50, 4)
```

El mensaje es `XMsgSystemProcessCall`, **no** `XMsgInProcessCall`. Pide un
`XObject` a associate: `0x0001D4C0` en la primera llamada, `0` en la segunda
— y esa segunda es un **desregistro** tomado en un camino de error, no el
registro de un segundo stream.

El dump de 8 dwords que hacía el handler leía fuera del mensaje:

| valor | lo que realmente es |
|---|---|
| `out[2] = 0x82A55BF8` / `0x82B05544` | **el link register guardado** — coincide exactamente con el retorno de cada llamada |
| `out[1] = 0x83AB9FA0` | `r1+0x54`, un local vivo del stack del título |

Consecuencia: el handler escribía un "NUI state object" inventado de ~28 KB
dentro del stack vivo (`out[1]+0x18`, `+0x70`, `+0x6CF0`) y guardaba `S_OK`
sobre `0x82A55BF8`, **la instrucción a la que iba a volver la CPU**.

**Regla**: respetar siempre `buffer_length`. Ahora `out[1]`/`out[2]` solo se
tocan si el título declaró ≥3 dwords. Kinect Adventures declara 4 → no se
escribe nada fuera del mensaje.

### El harness era circular

`SimpleSkeletonTracking` (XDK, requiere `nuiapi.h`/`AtgUtil.h`/`xtl.h`) y el
`.xex` custom "NuitestSelf" (fuente perdida) quedaron **eliminados**. Razones:

- Kinect Adventures hace **0 llamadas COM** (`NuiSkeletonGetNextFrame` y el
  resto de `INuiSensor` no tienen ni un call site). El harness probaba un
  camino que el título no usa.
- El `.xex` del harness pasaba un buffer de **32 bytes** y dependía
  exactamente del layout mal leído arriba, así que validaba el error circular:
  habría pasado igual contra las escrituras fabricadas.
- Sin el XDK no se puede recompilar; y `docs/kernel.md` confirma que Xenia
  implementa `xboxkrnl.exe`/`xam.xex` por su cuenta, así que el emulador no
  necesita el kit.

Nada del XDK estuvo nunca en git (`.gitignore:141` cubre el directorio
completo; `git log --all` no muestra ningún archivo del XDK).

### Trampa: NO borrar `build/bin/Windows/Release/content/`

Es la caché de contenido extraído del ISO, y en modo disc-image Xenia **no
la crea si falta**. Borrarla hace que `ExtractDeviceFile` lance
"Path Not Found" y el juego muera al pedir `update:\Database.xmplr`:

```
F> DiscImageDevice::ResolvePath(\update:\Database.xmplr)
i> XamContentOpenFile: path 'update:\Database.xmplr' not found
F> DiscImageDevice::ResolvePath(\Database.xmplr)
<Nested Exception Encountered>
```

Costó 3 corridas de 75 s descubrirlo. Si falta, `New-Item -ItemType Directory
build\bin\Windows\Release\content` y reextrae solo.

### Estado tras esta sesión

| | |
|---|---|
| Arranque | splash → calibración → GOOD! (T+46s) → IS ANYBODY THERE? (T+52s) |
| Escrituras fabricaradas | **0** (verificado en log) |
| Frames del driver | `frames=2251 guest_signals=2191` (nadie los consume todavía) |
| Bloqueo | sigue siendo "nadie consume los frames" |

Todo lo verificado con `tools/` (ver sección siguiente).

---

## Sesión 2026-10-16 (a): verificación de la evidencia previa + toolchain RE offline

> Esta sección **corrige** conclusiones anteriores de este documento y de
> `.agents/AGENTS.md` que se resultaron incorrectas al verificarlas contra los
> bytes del guest y el log de una corrida real. Confiar en ellas costó varias
> sesiones.

### El problema de método: RE a ciegas

Las sesiones anterioresibesinverse engineering del título **adivinando** qué
había en cada dirección del guest. De ahí salieron errores:

| Afirmación previa | Realidad verificada |
|---|---|
| Tabla "`0x835AF884` = `KeWaitForSingleObject`" | La región `0x835AF7D4+` es una tabla de thunks de 16 bytes. La tabla real se puede leer del log de Xenia. |
| `0x82D3065C` es un bucle infinito de reintento | `bne cr6, 0x82D305C0` va a un **error path** que loguea una cadena y retorna. No es bucle. |
| `out[5]` es "un puntero a código" | Es la cadena `"%s\%s"` (ya corregido en una sesión previa; se repite aquí por completitud). |
| `XamNuiIdentityGetSessionId` es el gate bloqueante | Ese codigo nunca se ejecuta: ver "Hallazgo decisivo" abajo. |

### Toolchain nuevo (`tools/`) — RE offline en segundos

Antes: cada hipótesis costaba un run de 60 s del juego + OCR. Ahora:

```powershell
# 1. mapa nombre -> thunk del guest, sac��do del log de Xenia (un solo run)
python tools\thunks.py build\bin\Windows\Release\xenia.log > thunks.txt

# 2. quié́n llama a un export del kernel/XAM, offline, sobre el dump de 32 MB
python tools\xrefs.py guest_nui_dump.bin thunks.txt --all-nui

# 3. desensamblado con alineación exacta
python tools\xex.py guest_nui_dump.bin dis 0x82D3065C 40
```

`tools/xex.py` carvea la ventana a un archivo scratch y lo mapea en la vaddr
exacta. **No usar r2 sobre el XEX crudo**: con la PE cargada r2 aplica sus
propias heurísticas de alineación y decodifica las instrucciones corridas
(`bl XamNuiIdentityGetSessionId` en `0x82D3065C` se renderiza como
`lwz r6, 0x48(r8)`), y con `-B 0 -m <base>` mapea mal el offset.

### Ground truth del log de la última corrida real

El build es fresco: `xenia_canary.exe` (21:55:53) es posterior a `xam_nui.cc`
(21:53:08), así que el log refleja el WIP actual.

```
XamNuiGetDeviceStatus called (n=1, tid=00000006)   <- UNA sola vez
XamUserNuiGetUserIndex                            <- 0 llamadas (solo aparece en el import dump)
```

- La tabla de imports tiene **351 entradas, 32 sin implementar**.
- El título llama 10 veces a `XamXStudioRequest` (el canal real del driver NUI).

### Hallazgo decisivo: los gates biometricos son un callejón sin salida

`XamUserNuiGetUserIndex` **nunca se invoca**. Por lo tanto **nunca se ejecuta**
ni el gate que lo contiene (`0x82D306B8`) ni el gate de session id que lo
antepone (`0x82D3065C`). Toda la línea de investigación "puertas de presencia /
biometría / session id"描述 un camino que el título no recorre.

El código verificado en `0x82D30630`:

```asm
0x82d30608  bl  0x82d3c990      ; llena ctx+0x249A0..B0 (incluye el session id esperado)
0x82d3060c  cmpwi r3, 0
0x82d30614  bge  0x82d30644
0x82d30644  ...
0x82d3065c  bl  0x835afbd4      ; XamNuiIdentityGetSessionId()
0x82d30660  lis  r10, 2
0x82d30668  ori  r10, r10, 0x49a8
0x82d3066c  ldx  r10, r11, r10 ; r10 = session id esperado en el ctx del título
0x82d30670  cmpld cr6, r3, r10
0x82d30674  bne  cr6, 0x82d305c0  ; -> ERROR (log + return), no loop
```

### La escritura fabricarada en `0x83896510` es inerte

`xam_app.cc:628` registra una "master device queue object" en una **dirección
guest fija y adivinada** (`0x83896510`), y el driver le escribe un **layout
inventado** (`+0x00`, `+0x58`, `+0xA40`, `+0x70`) cada frame.

- En el dump de runtime la región está **enteramente en ceros** (0x200 bytes).
- El título nunca recibe un puntero a ella, así que no puede observarla.

Es el mismo patrón que ya causing un freeze en el bisect del 2026-10-13
("nunca fabricar destinos de escritura por heurística"). No aporta nada y
debería salir o pasar detrás de un cvar apagado por defecto.

### Estado real del bloqueo

El título arranca, pasa splash + calibración, y se queda en "IS ANYBODY THERE?".
El driver produce frames (`frames=2551 guest_signals=2487`) y señala los eventos
registrados, pero **nadie los consume**: el título importa **ninguna** función
`Nui*` de esqueleto (confirmado por el barrido de xrefs). Los esqueletos tienen
que llegar por el servicio NUI de XAM.

Próximo paso correcto: entender qué espera el título en `XamXStudioRequest`
(cmd `0x1502` en `0x82D35720`, y `cmd=6`) y en el **segundo** mensaje `0x2B001`
—que hoy ignoramos porque `out[1]=0x83A20000` llega sin inicializar— en vez de
perseguir los gates de biometría.

---

## Resumen ejecutivo

El pipeline de Kinect emulado en Xenia está **verificado end-to-end** con un title
de prueba propio (ver "Test Harness" más abajo). El título comercial Kinect
Adventures arranca correctamente (toda la secuencia de calibración), pero se
detiene en la pantalla *"IS ANYBODY THERE?"* porque consume los esqueletos por
una ruta distinta a la que expusimos.

---

## Estado por capa

| Capa | Estado | Evidencia |
|---|---|---|
| `KinectInputDriver` — producción de frames a 30fps | ✅ | `frames=2251 guest_signals=2190 tracked_state=2` |
| Frames sintéticos (TRACKED, ID, posición, joints) | ✅ | `skeleton[0] id 1 pos (0.00, 0.00, 2.00)` |
| Serialización big-endian hacia memoria guest | ✅ | Guest BE lee los valores directamente |
| Bind COM vía `XamApp` `0x2B001` | ✅ | `device=3002A000`, `hr=0x00000000` |
| vtable COM → `NuiSkeletonGetNextFrame` (slot 18) | ✅ | 555 frames, 0 failures |
| Kinect Adventures — calibración completa | ✅ | STAND UP TO PLAY → FACE THE SENSOR → GOOD! |
| Kinect Adventures — pasar a gameplay | ❌ | Se detiene en "IS ANYBODY THERE?" |

---

## Bugs del emulador encontrados y corregidos

### 1. `NuiSkeletonGetNextFrame` consumía el frame (single-consumer)

El driver exponía un flag `new_frame_` que se limpiaba al leer. Es un API de
**polling** con varios lectores (trampoline COM, diagnósticos, loop de render del
juego): el que perdía la carrera recibía un frame **cero** en lugar del actual.

**Causa raíz de que los juegos no vieran el esqueleto** pese a "producirlo" a 30fps.

**Fix**: `kinect_input_driver.cc` — el frame ya no se consume; siempre se devuelve
el último disponible.

### 2. `KeSetCurrentProcessType` descartaba el valor

`xeKeSetCurrentProcessType` solo escribía en el scratch del PCR cuando el contexto
estaba en DPC; fuera de DPC la llamada era un no-op silencioso.

**Fix**: `xboxkrnl_threading.cc` — fuera de DPC escribe en el
`X_KTHREAD::process_type` del thread actual, que es de donde lo lee
`KeGetCurrentProcessType`.

---

## Búsqueda de la causa raíz del freeze (resuelto por bisect)

El título se colgaba en el splash "Microsoft". Bisect del working tree contra
HEAD limpio (`97de7aacd`), build + test OCR de 75s por iteración:

| Configuración | Resultado |
|---|---|
| HEAD puro | ✅ boot OK |
| WIP completo | ❌ freeze |
| WIP sin instrumentación de kernel | ❌ freeze |
| + revert de `memory.cc`, `utf8.cc`, `xam_avatar`, `xam_voice`, `disc_image_device` | ❌ freeze |
| + revert de `xam_content.cc` | ❌ freeze |
| + revert de `kinect_input_driver.{cc,h}` **y** `xam_app.cc` | ✅ boot OK |
| + WIP del driver solo | ✅ boot OK → **el culpable era `xam_app.cc`** |
| WIP de `xam_app` sin broadcasts NUI | ❌ freeze |
| WIP de `xam_app` **sin registro de "shared skeleton buffers"** | ✅ **boot OK** |

**Causa raíz**: el handler de `0x2B001` registraba los punteros `out[5]`/`out[7]`
del mensaje como "shared skeleton buffers" usando una heurística de rango de
direcciones (`>= 0x83000000`), y el poll thread escribía ~2.6 KB de datos de frame
en ellos cada 33 ms. Esas direcciones **no son** buffers de frames — `out[5]` se
observó conteniendo un puntero a código (`0x8213D660`) — así que la escritura
corrompía memoria guest viva.

**Regla derivada**: nunca fabricar destinos de escritura por heurística de forma
de dirección. Los frames se entregan exclusivamente por el trampoline COM
`NuiSkeletonGetNextFrame`.

**Diagnóstico descartado**: la teoría previa de "producer/consumer stall en el
evento `F800002C`" era un artefacto del estado corrompido, no un bug de
scheduling. Toda la instrumentación asociada (wait probe, dumps de GPR/pila) se
revertió.

---

## Test Harness (`NuitestSelf`)

Título de prueba propio que ejercita exactamente la ruta que emulamos,
**sin** depender del runtime NUI del sistema:

1. `XMsgInProcessCall(HXAMAPP_XAM, 0x0002B001, buffer, size)` → pide el device
2. Llama la vtable COM directamente: slot 18 = `NuiSkeletonGetNextFrame`
3. Decodifica el frame big-endian y dibuja estado + esqueleto

El título compilado y sus assets están en `SimpleSkeletonTracking/Release/`
dentro de este repo. El `.xex` resultante se lanza directamente con
`xenia_canary.exe <ruta>\<titulo>.xex`.

> **Local only**: el harness compilado y sus artefactos están en `.gitignore`
> (decisión explícita). Si alguien más necesita reproducirlo, hay que reconstruir
> el `.xex` con el toolchain de desarrollo.

Salida observada:

```
NuiTest - direct INuiSensor pipeline          0.00 fps
ticks: 555              bind hr: 0x00000000
NuiStatus: 0x00000000   device: 3002A000
frames ok: 555          failures: 0
tracked skeletons: 1
frame #555  timestamp 14017874862
skeleton[0] id 1  pos (0.00, 0.00, 2.00)
head (0.00, 0.00, 2.00)  hips (0.00, 0.60, 2.00)
```

Notas de implementación específicas del harness:
- El toolchain es C++03: sin `constexpr`, sin initializers in-class.
- El CPU es big-endian → los datos de `store_and_swap` se leen sin swap.
- `Application::Run()` del framework no presenta; hace falta
  `IDirect3DDevice9::Present()` explícito.
- `Font::DrawText()` hace su propio begin/end; un `Begin()` extra rompe el
  batching y no dibuja nada.

---

## Análisis del runtime NUI del título (desensamblado)

Se volcó la imagen descomprimida del título a `guest_nui_dump.bin` (32 MB, base
`0x82000000`) desde `XamNuiGetDeviceStatus`, que los títulos que usan el servicio
NUI llaman exactamente una vez y justo antes de que sus workers se parked. Eso da
un punto de captura determinista para desensamblar offline.

Herramienta: `radare2` con `r2 -a ppc -b 32 -m <base> -e cfg.bigendian=true`.

### Threads NUI del título

Los workers arrancan en `0x82C48D18` (entry del runtime NUIPRE embebido en el
propio título, no en xam.dll). TID 9 y TID 0xB.

Flujo desensamblado de `fcn.82c48d18`:

```
loop:
  KeWaitForSingleObject(ev, 3, 1, 0)   # 0x835af884
  if (result == 0x102 /* STATUS_TIMEOUT */):
      goto loop                          # timeout normal: reintentar
  KeResetEvent(ev)                       # 0x835af9b4
  if (field_4 == 0):
      return                             # nada pendiente
  ProcessQueue(list_node)                # 0x82c48a58
```

`ProcessQueue` recorre una **lista enlazada de pendientes** (`lwz r11,0x3c(r31)`
+ `cmpwi r11,0` hasta el final) y por cada item llama
`vtable[4]` del objeto NUI (`lwz r11,0x10(r31)` + `mtctr` + `bctrl` en `0x82c48bf0`).

### Identificación de llamadas por import table

| Dirección | Símbolo |
|---|---|
| `0x835AF884` | `KeWaitForSingleObject` |
| `0x835AF9B4` | `KeResetEvent` |
| `0x835AF9C4` | `KeGetCurrentProcessType` |
| `0x835AF7E4` | `RtlEnterCriticalSection` |
| `0x835AF804` | `RtlLeaveCriticalSection` |

### Conclusión

El worker **nunca despierta** porque el evento que espera (`F800002C`) lo crea el
propio título *después* del bind `0x2B001` y nunca es señalado. El bind entrega:

```
out[0]=0x0001D4C0  event        → registrado como skeleton frame event ✅
out[1]=0x83AB9FA0  NUI object   → se le escribe el device en +0x18 ✅
out[2]=0x82A55BF8  hr_ptr       → S_OK ✅
out[3]=0x4012B7D0  callback    → ignorado ❌
out[5]=0x8213D660  (código)     → ignorado, correcto ✅
out[7]=0x83D30000  frame buffer → registrado ✅
```

Los eventos que el título crea después (handles `F80000xx`) no se registran en
ningún lado del emulador, así que el pipeline de señalización no tiene a quién
avisar. Falta un mecanismo de *reactive signal*: cuando el driver produce un
frame, además del evento del skeleton, hay que avisar a los objetos NUI que el
propio título creó para su cola de trabajo.

## Pendiente: ruta XAM-NUI de Kinect Adventures

El título **no** usa la ruta COM que expusimos. Evidencia del log:

```
XamNuiGetDeviceStatus called (n=1, tid=00000006)     ← pregunta UNA vez
KinectInputDriver: NuiInitialize OK (backend=Synthetic)
...
KeWaitForSingleObject: tid=00000009 obj=F800002C      ← worker NUI espera para siempre
XMsgInProcessCall: app=000000FE msg=00021012 ...      ← polling de signin cada frame
```

- `XamNuiGetDeviceStatus` se llama 1 vez → devolvemos `status=0x45` (sensor OK)
- **0 invocaciones** de `NuiSkeletonGetNextFrame` en toda la corrida
- Los workers NUI (tid 9/0xB) quedan bloqueados en el evento `F800002C`
- El driver sigue produciendo frames y señalizando (`guest_signals=2190`) pero
  **nadie los consume**

Lo que falta para esta ruta:
1. Que `XamNuiGetDeviceStatus` dispare el bind del device COM (o exponga el
   handle por donde el juego lo lee).
2. Identificar qué señala `F800002C` y en qué momento del handshake — es el
   evento que los workers esperan y nunca se señaliza.
3. Verificar el polling `0x21012` (XamEnableGuestSignin): el juego lo llama cada
   frame; puede estar esperando una transición de estado que nunca ocurre.

---

## Instrumentación temporal activa (pendiente de limpiar)

- `DbgBreakPoint`: ahora log + continue (con cvar `dbg_break_on_guest_breakpoint`
  para restaurar el break). Cvars emulan hardware sin debugger.
- `xboxkrnl_debug.cc`: dump de string de error y dump de 4MB de la imagen guest.
- `NtCreateFile`: probe logging de las primeras 200 aperturas con su status.
- `NuiCOMStub_SkeletonGetNextFrame`: dump periódico del estado del frame.
- `XamLoaderTerminateTitle`: log del caller y código circundante.

## Recursos del host pendientes de limpieza

Estos se usaron solo para construir el harness de prueba y **no son parte del
proyecto** (no referenciarlos desde el código ni los docs de Xenia):

- `C:\Users\Carlos\Projects\test_samples\` — árbol de samples compilado. El
  proyecto referencia únicamente `SimpleSkeletonTracking/Release/*.xex` (el .xex
  es un artefacto compilado, autocontenido).
- Descargas en `C:\Users\Carlos\Downloads\`: instaladores del toolchain de
  desarrollo (`SDKSetupXenon.exe`, `VS2010Express1.iso`,
  `VS2010Pro.iso`), la ISO de recovery (`21256.3_Xenon_Recovery.iso`) y la carpeta
  `NUIView\`. Todos descartables.
- Instalaciones en el sistema: `Microsoft Xbox 360 SDK` y `herramientas de compilacion`.

### Notes de compilación del harness
Para reconstruir el `.xex` hace falta el toolchain de desarrollo instalado
(el compilador de Xbox 360) y compilar la solución con:

```
MSBuild.exe <sln> /p:Configuration=Release /p:Platform="Xbox 360"
```

El `.xex` compilado es determinista respecto del fuente: no depende de la
instalación en runtime, así que se puede archivar el `.xex` y descartar el resto.
En este repo **no** se archiva: queda ignorado por git.

## Cómo volver a testear rápido

```powershell
# Test del pipeline (arranca en ~5s, feedback inmediato)
build\bin\Windows\Debug\xenia_canary.exe SimpleSkeletonTracking\Release\SimpleSkeletonTracking.xex
# -> pantalla verde = tracked skeleton, rojo = sin skeleton

# Título comercial (llega a la pantalla de detección en ~50s)
python monitor_game_1s_ocr.py
```
---

## Hallazgo decisivo: el titulo NO lee `out[7]`

Hasta aqui la hipotesis era "el titulo lee el esqueleto del buffer compartido que
nos pasa en `out[7]`". **Es falso.** Dos experimentos independientes lo prueban.

### Experimento 1 -- centinelas auto-descriptivos

Se escribe en cada dword del buffer el valor `0x5A000000 | offset_dword`, que
codifica su propia posicion. Despues se vuelca 32 MB de RAM del guest y se
buscan las secuencias de centinelas.

```
guest_probe_b.bin: nuestro buffer 0x83D30000 -> si contiene los centinelas
                   secuencias de >=4 dwords fuera del buffer: 0
```

El titulo **no copia ni un solo bloque** de ese buffer a ningun otro sitio de su
memoria.

### Experimento 2 -- pagina de guardia

Se llenan los centinelas una vez y despues se quita todo el acceso a la pagina
(`kNoAccess`). Si el titulo cargara desde ahi, suffersia un page fault.

```
i> KinectInputDriver: guard page installed on shared frame buffer -> true
... 37.000 lineas de log posteriores: ni una excepcion, ni una violacion
```

`kNoAccess` si se aplica de verdad (Xenia lo usa igual para liberar heap), asi que
la conclusion es solida: **el titulo nunca lee esa direccion**.

### Por que se colgaba entonces

No era el layout del esqueleto. Era **nosotros pisando el heap**: el volcado de
centinelas escribia 8 KB seguidos en una reserva mas pequeña, corrompiendo
estructuras vecinas. El freeze nunca tuvo nada que ver con los datos de esqueleto.

### Donde NO llegan los datos

| Via | Estado |
|---|---|
| vtable COM del sensor | **0 llamadas** del titulo |
| buffer `out[7]` | **0 lecturas** del titulo |

El titulo nunca ha recibido datos de esqueleto por ningun camino. Por eso se
queda en "IS ANYBODY THERE?" y por eso nunca aparece la calibracion de tilt: esa
pantalla solo aparece *despues* de detectar un cuerpo, y no hay cuerpo porque no
hay fuente de datos.

### Decisiones que se caen

- Dejar de escribir en `out[7]` (o dejar el cvar en false por defecto).
- Dejar de asumir el layout `X_NUI_SKELETON_FRAME` en esa direccion.

### Siguiente paso

Identificar que objeto recibe el `vtable[4]` que el worker del titulo invoca en
`0x82C48BF0` tras despertar del evento, y confirmar si es el sensor mock o un
 objeto del propio runtime NUI del titulo. Si es nuestro, el problema es que el
titulo no esta usando nuestra vtable; si no lo es, el dato tiene que entrar por ahi.

## Mejoras del arnes de pruebas

`monitor_game_1s_ocr.py` ahora corta solo, sin esperar el final:

- `--seconds` / `--interval` en vez de valores fijos.
- `--freeze-exit N` corta cuando N capturas consecutivas son identicas a nivel de
  pixeles. Antes un congelamiento costaba los 75 s completos.
- `--warmup N` ignora el arranque (la pantalla de carga es estatica).
- `--stop-on-text ...` para terminar en cuanto salga un texto objetivo.
- `--cvar nombre=valor` para pasar config vars por linea de comandos. Xenia
  reescribe su `.toml` al arrancar y pisaba cualquier edicion manual, por eso los
  cvars nuevos nunca se activaban.
- El reporte se escribe linea a linea y se hace flush, asi que una corrida
  interrupted no pierde el timeline.

---

## Reconstruccion del contrato real de NUI (2026-10-14)

### El titulo no importa NINGUNA funcion Nui* de esqueleto

Barrido completo de la import table del titulo:

```
XamNuiGetDeviceStatus      XamNuiCameraTilt*       XamNuiCameraElevation*
XamNuiIdentityGetSessionId XamUserNuiGetUserIndex  XamUserNuiEnableBiometric
XamShowNui*UI
```

No hay `NuiSkeletonGetNextFrame`, ni `NuiCreateSensor*`, ni `NuiImageStreamOpen`.
Los esqueletos llegan al titulo por el servicio NUI de XAM, no por COM. Por eso
nuestro sensor mock nunca recibe una llamada: no es que el titulo lo ignore, es
que **nunca pide uno**.

### Los argumentos del mensaje 0x2B001 NO son lo que creiamos

Volcado de los 8 argumentos en el momento de la llamada:

| arg | valor | contenido real |
|---|---|---|
| out[0] | `0001D4C0` | handle (evento) |
| out[1] | `83AB9FA0` | struct de params/camara del titulo (floats, flags) |
| out[2] | `82A55BF8` | **array de floats**, no un HRESULT |
| out[3] | `4012B7D0` | descriptor RTTI del XEX (`"clamp"`, `"clampx"`) |
| out[4] | `7018F5C0` | descriptor del lado XAM |
| out[5] | `8213D660` | **cadena** `"%s\%s"`, no un puntero a codigo |
| out[6] | `FFFFFFFF` | -1 |
| out[7] | `83D30000` | heap, nunca leido |

Corregir dos errores previos del analisis:
- `out[5]` no es "un puntero a codigo": es la cadena de formato `"%s\%s"`.
- `out[2]` no es un `hr_ptr`: es un array de floats (0.047, 0.034, 0.031, ...).

Escribir en `out[5]` por eso no era "corrupcion de codigo" sino escribir encima
de una cadena en .rdata.

### El titulo consulta el estado UNA vez y se bloquea

```
i> XamNuiGetDeviceStatus called (n=1, tid=00000006)
```

Una sola llamada, en el hilo principal, ya al inicio. Devolvemos `status=0x45` y a
partir de ahi el titulo aparca sus hilos worker en un evento propio.

### 0x2B002 nunca se envia

Tenemos implementado `case 0x0002B002` ("skeleton tracking enable") y **nunca
llega**. XAM decide no activar el stream de esqueletos.

### La notificacion de hardware abre una segunda puerta

El codigo tenia un comentario desde un bisect anterior: emitir
`kXNotificationSystemNUIHardwareStatusChanged` (0x00060019) desde el handler de
0x2B001 colgaba el titulo en el splash. Probado **con retardo** (10 s, ya pasado
el splash): no cuelga, y **dispara una segunda llamada a 0x2B001** que antes no
existia:

```
broadcast NUIHardwareStatusChanged state=1 at t+10s
0x2B001 raw buffer: 00000000 83A20000 82B05544 00000000 7018F6B0 00000000 00000000 00000000
```

Diferencias con la primera llamada:

| arg | 1a llamada | 2a llamada |
|---|---|---|
| out[0] evento | `0001D4C0` | `00000000` |
| out[1] objeto | `83AB9FA0` (inicializado) | `83A20000` (basura `3F499329`) |
| out[3] | `4012B7D0` | `00000000` |
| out[4] | `7018F5C0` | `7018F6B0` |
| out[7] buffer | `83D30000` | `00000000` |

`out[4]` de la 2a llamada es **el segundo campo** (`+0`) de la estructura que la
1a pasada paso en `out[4]`: los dos son entradas de la misma lista de
descriptores de stream de XAM. Es decir, la 2a llamada es **registro de otro
stream**, muy probablemente el de esqueletos.

**Y nosotros la ignoramos por completo**: ellog dice
`status_ptr 83A20000 has non-object header 3F499329, skipping ready_flag write`.

### Estado

La notificacion con retardo no colgela y es segura (se deja activa, cvar
`nui_notify_hardware_delay`, -1 para desactivar). Pero el juego sigue volviendo a
"IS ANYBODY THERE?" en T+51s, porque la segunda llamada a 0x2B001 queda sin
responder.

### Siguiente paso concreto

Responder correctamente la 2a llamada a 0x2B001. Lo que hay que averiguar es el
layout de la struct que hay que escribir en `out[1]` (0x83A20000), que llega sin
inicializar. Pistas a seguir:
- `out[4]` apunta a la lista de descriptores de stream de XAM: leer esa
  estructura revela cuantos streams hay y de que tipo, y por tanto cual es el de
  esqueletos.
- El titulo escribe/lee `out[1]` en `+0x7C` (4 -> 5 -> 0), `+0x8C`, `+0xA8`,
  `+0xBC` (contador que llega a 0x4D5 = 1237): son los unicos campos que toco.

---

## Sesión 2026-10-15: Decompilación exacta de XAM NUI y compuertas de presencia

### 1. Resolución de fallas de imports XAM / Kernel
- **`XamXStudioRequest` (`0x835AFB04`)**: Decompilados los 10 llamadores en guest (`cmd=1, 6, 0x301, 0x302, 0x1003, 0x1004, 0x1401, 0x1402, 0x1502`). Implementada respuesta con `X_ERROR_SUCCESS` en `xam_nui.cc`.
- **`EtxProducerRegister` (`0x835AF8C4`)**: Registrado en `xboxkrnl_debug.cc` con `X_STATUS_SUCCESS`.
- **Resultado visual**: Juego supera splashes y calibración llegando limpiamente a "IS ANYBODY THERE?" a T+51s.

### 2. Compuertas de estado identificadas por ingeniería inversa

1. **Gate de perfil de usuario (`0x82B76178`..`0x82B761B0`)**:
   - `0x82B76178` llama `XamUserGetSigninInfo(0, &info)`.
   - `0x82B76188`: `cmpwi cr6, r11, 2` — verifica si `signin_state == SignedInToLive` (2).
   - Si no es 2, escribe `-1` en `0x4BC(r31)` desconectando al jugador cada tick.
   - **Acción requerida**: En `user_profile.h`, hacer que `signin_state()` devuelva `SignInState::SignedInToLive` (2).

2. **Gate de detección de esqueleto / biometría (`0x82D306B8`, `0x82D31E38`)**:
   - El bucle biométrico del juego llama `XamUserNuiGetUserIndex(tracking_id, &user_index)`.
   - En `0x82D306BC` y `0x82D31E3C`: comprueba que el retorno sea `>= 0` (`X_STATUS_SUCCESS`).
   - Al tener éxito, `0x82D306E0` almacena `1` en `0(r30)` (`is_player_engaged = 1`), disparando el callback de presencia.
   - `XamUserNuiGetUserIndex` estaba marcado como `kStub` en `xam_nui.cc`.

3. **Status bits de dispositivo (`XamNuiGetDeviceStatus`)**:
   - `0x82A5F8F0`: Comprueba bit 30 (`0x02` = Sensor Ready). Al cumplirse pasa a State 4 (Tracking Mode).
   - `0x82B04324`: Comprueba bit 29 (`0x04` = Hardware Error). Si está activo llama a `XamShowNuiHardwareRequiredUI`.
   - El valor `status = 0x47` en `XamNuiGetDeviceStatus` satisface ambos requisitos.

4. **Gate de NUI Identity Session ID (`0x82D3065C`..`0x82D30674`)**:
   - `0x82D3065C`: Llama a `XamNuiIdentityGetSessionId()`.
   - `0x82D3066C`: Carga el session ID esperado desde la memoria guest en `*(r11 + 0x000249A8)`.
   - `0x82D30670`: `cmplw cr6, r3, r10` compara el retorno con el valor esperado.
   - `0x82D30674`: `bne cr6, 0x82D305C0` — Si no coinciden, salta hacia atrás en un bucle infinito, impidiendo que el flujo alcance la llamada a `XamUserNuiGetUserIndex` que activa `is_player_engaged = 1`.
   - En `xam_nui.cc`, `XamNuiIdentityGetSessionId` devuelve `0` fijo, lo que causa este spin loop continuo en el hilo de biometría.


