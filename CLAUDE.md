# FiveCom — Voice Chat P2P (Windows / C++ / WASAPI)

Aplicación nativa Windows 64-bit para chat de voz tipo Discord/TeamSpeak, con
arquitectura P2P donde el primer usuario en crear un canal es el **Master**
(actúa como server) y los demás se conectan como **Clientes**. Diseñada para
consumir lo mínimo posible (sin Electron, sin GC, audio nativo WASAPI, codec
Opus).

---

## Build

**Pre-requisito único**: instalar Opus (una sola vez):

```bash
D:/msys64/usr/bin/pacman.exe -S --noconfirm mingw-w64-x86_64-opus
```

**Compilar** (desde el directorio raíz `D:\Coder\fivecom`):

```bash
export PATH="/d/msys64/mingw64/bin:$PATH"
/d/msys64/usr/bin/make.exe
```

Produce `fivecom.exe` (~1.6 MB, totalmente estático: sin dependencias de DLLs
del runtime de mingw).

`make clean` borra los `.o` y el ejecutable.

---

## Cómo se usa

**PC-A (Master)**
1. Ejecutar `fivecom.exe` → escribir nick → click **Crear canal**
2. La barra de estado muestra tu IP local (`192.168.x.y:7777`)

**PC-B (Cliente)**
1. Ejecutar `fivecom.exe` → escribir nick → escribir la IP del Master en *IP Master*
2. Click **Unirse**

Botones: **Mutear** silencia el mic, **Salir** desconecta del canal.

Indicador `[*]` al lado de cada nick: detección simple de actividad por RMS.

Puerto: **UDP 7777** (constante en `network/packet.h`).

---

## Arquitectura

### Topología

```
Cliente A ──┐
Cliente B ──┼──► Master (mezcla mix-minus + redistribuye)
Cliente C ──┘
```

- El Master abre un socket UDP en el puerto 7777
- Cada Cliente envía su mic al Master
- El Master mezcla todas las voces y reenvía a cada peer un mix donde **se
  excluye la voz del propio peer** (mix-minus, evita escucharse en eco)
- El Master también escucha localmente lo que dicen los demás (sin su propia voz)

### Stack

| Componente | Tecnología | Razón |
|---|---|---|
| Audio I/O | **WASAPI shared mode** + `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` | Nativo de Windows, mínimo overhead, Windows hace el resampling automático |
| Codec | **Opus 1.6.1** (24 kbps, DTX, FEC inband) | Mejor codec para voz, suprime silencio |
| Red | **WinSock2 UDP** | Latencia mínima, sin handshake |
| GUI | **Win32 puro** | Sin Electron, sin Qt, ~0% overhead |
| Build | **g++ 15.2.0** (MinGW64), Makefile | En `D:\msys64\mingw64\bin\g++.exe` |

### Modelo de hilos

| Hilo | Prioridad | Función |
|---|---|---|
| Main / GUI | Normal | Win32 message loop + timer 200ms para refresh UI |
| `WasapiCapture` | Pro Audio (MMCSS) | mic → callback con frames de 960 muestras (20ms) |
| `WasapiRender` | Pro Audio (MMCSS) | parlantes ← callback que pide frames de 960 muestras |
| `MasterNode::net_loop` | Normal | UDP recv → demux por tipo de paquete |
| `MasterNode::tick_loop` | Normal | cada 20ms: pop jitters → mezclas → encode → send |
| `ClientNode::net_loop` | Normal | UDP recv → decode → push a jitter |

---

## Estructura de archivos

```
D:\Coder\fivecom\
├── Makefile                       Build (TMP redirect a .tmp local)
├── CLAUDE.md                      Este archivo
├── fivecom.exe                    Binario final
├── fivecom.log                    Log de runtime (se reescribe en cada arranque)
└── src\
    ├── main.cpp                   WinMain + GUI Win32 + handlers de botones
    │
    ├── audio\
    │   ├── wasapi.h/.cpp          WasapiCapture, WasapiRender (init sync,
    │   │                          AUTOCONVERTPCM, MMCSS, callback con
    │   │                          FRAME_SAMPLES garantizado)
    │   └── codec.h/.cpp           OpusEncoderW, OpusDecoderW, JitterBuffer
    │
    ├── network\
    │   ├── packet.h               Header binario + tipos JOIN/AUDIO/PING/etc
    │   └── socket.h/.cpp          UdpSocket (WinSock2 + select timeout) +
    │                              detección de IP local (GetAdaptersAddresses)
    │
    └── core\
        ├── app.h/.cpp             AppState singleton (mode, peers, mute, etc)
        ├── log.h/.cpp             Logger thread-safe a fivecom.log
        ├── master.h/.cpp          MasterNode (peers, mix-minus, broadcast)
        └── client.h/.cpp          ClientNode (mic→encode→send, recv→decode→play)
```

---

## Pipeline de audio (detalle)

### Captura (cliente)
```
WASAPI capture → buffer del dispositivo (típ. 480 samples = 10ms)
                ↓
            accum vector
                ↓ (cuando >= 960 samples)
         callback(960 samples 16-bit mono 48kHz)
                ↓
         detect_speech (RMS > 500)
                ↓
        Opus encode (24kbps, DTX)
                ↓
         UDP send al master
```

### Master (mezcla mix-minus)
Cada 20ms el `tick_loop`:
1. Pop un frame de cada `peer.jitter` (decodificado por net_loop)
2. Pop un frame de `mic_jitter_` (mic local del master)
3. Para **cada peer destino P**:
   - `mix = mic_master + Σ(otros_peers) − P`
   - Encode con `P.enc` → send a `P.ep`
4. Para parlantes locales del master:
   - `local_mix = Σ(todos los peers)` (sin propia voz)
   - Push a `local_play_jitter_`

### Render (cliente o master)
```
UDP recv PKT_AUDIO → Opus decode → push jitter
                                       ↓
WASAPI render → callback(960 samples) → pop jitter → write a buffer device
```

---

## Formato de paquetes (UDP)

```
PacketHeader (13 bytes, packed):
  uint32  magic    = 'FCOM' (0x46434F4D)
  uint8   type     // PKT_JOIN/PKT_JOIN_ACK/PKT_AUDIO/PKT_LEAVE/PKT_PING/PKT_PEER_LIST
  uint32  peer_id  // 0 si aún no asignado, 1 = master
  uint16  data_len // bytes de payload que siguen
```

Tipos:
- **JOIN** (cliente→master): `JoinPayload { char nick[32] }`
- **JOIN_ACK** (master→cliente): `JoinAckPayload { uint32 assigned_id, char master_nick[32] }`
- **AUDIO**: payload Opus comprimido (~50–150 bytes)
- **PEER_LIST** (master→clientes): array de `PeerListEntry { uint32 id, char nick[32] }`
- **PING**: keepalive vacío, cada 2s desde cliente
- **LEAVE**: cliente avisa que se va (best-effort)

Timeout de peer en master: 10s sin actividad → drop + republish_peer_list.

---

## Bugs encontrados durante el desarrollo (lecciones)

1. **Render con buffer del device chico**: WASAPI pide samples de a ~480 (un
   "device period" de 10ms a 48kHz), pero el callback procesaba en bloques de
   960 (20ms — tamaño de frame Opus). El loop `filled + 960 <= 480` nunca era
   true → todo silencio. **Fix**: el `WasapiRender` ahora mantiene un buffer
   intermedio (`stage`) y siempre llama al callback con `FRAME_SAMPLES`,
   re-acumulando lo que sobra para el próximo `to_fill` de WASAPI.
   ([src/audio/wasapi.cpp](src/audio/wasapi.cpp#L266-L299))

2. **Sample rate mismatch**: el código original requería que el dispositivo
   estuviera exactamente a 48kHz, falla silenciosa si era 44.1kHz. **Fix**:
   `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` + `AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`.
   Windows 7+ se encarga del resampling automáticamente.

3. **Init asincrónico oculta errores**: `start()` retornaba true antes de que
   el thread hiciera `IAudioClient::Initialize`. **Fix**: handshake con
   `init_evt_` (CreateEvent + WaitForSingleObject 5s); `start()` retorna
   `false` con el HRESULT real si la init falla.

4. **GUIDs `KSDATAFORMAT_SUBTYPE_*` undefined al linkear**: faltaba
   `#define INITGUID` antes de `<initguid.h>` en una unidad. **Fix**: agregado
   en `src/audio/wasapi.cpp`.

5. **`WinMain` undefined**: `wWinMain` (UTF-16) requiere `-municode` en el
   linker para que MinGW use el stub correcto.

6. **TMP=C:\WINDOWS\ no escribible**: g++ creaba temp files ahí por default.
   **Fix**: el Makefile exporta `TMP=D:/Coder/fivecom/.tmp`.

---

## Logging y diagnóstico

- `fivecom.log` se reescribe en cada arranque
- Cubre: entry/exit de WASAPI init (con HRESULTs), clicks de botones, fallas
  de network bind, etc.
- Crashes SEH (segfaults, etc.) se capturan con
  `SetUnhandledExceptionFilter` y muestran un MessageBox con código + dirección
- Para debug deeper, descomentar más `log_msg(...)` en master/client/wasapi

---

## Limitaciones y bugs conocidos

| Item | Estado | Notas |
|---|---|---|
| **Eco en el mic de PCs de escritorio** | Conocido, sin AEC | Pasa cuando mic y parlantes están en la misma sala. Las notebooks lo evitan por AEC del driver/hardware. La PC de escritorio del autor lo tiene. |
| **Solo LAN** | Por diseño del prototipo | Sin NAT traversal. Funciona dentro de la misma red local. |
| **Canal único** | Por diseño del prototipo | Una sala por instancia del Master. |
| **Sin autenticación / cifrado** | Aceptable para prototipo LAN | Cualquiera en la red puede conectarse y escuchar/hablar. |
| **Sin push-to-talk global** | Falta | Hoy es always-on (con DTX cortando silencio). |
| **Sin selector de dispositivo** | Falta | Usa siempre el default de Windows (Communications role; si falta, Console role). |
| **Sin ajuste de volumen por peer** | Falta | Todos se mezclan al mismo nivel. Con muchos peers puede saturar (clip16 a int16). |
| **Detección de voz fija (RMS > 500)** | Demasiado simple | No es VAD real. Si el mic está muy bajo, puede no marcar speaking. |
| **Limitación de paquete: 1500 bytes** | OK en LAN | Suficiente para Opus a 24kbps. |

---

## Roadmap / próximos pasos sugeridos

### Cercanos (mejoras de UX y calidad)

1. **Cancelación de eco (AEC)** — para resolver el eco del autor
   - Opción A: integrar `webrtc-audio-processing` (paquete MSYS2:
     `mingw-w64-x86_64-webrtc-audio-processing`). Provee AEC + NS + AGC + VAD
     en una sola lib.
   - Opción B: usar `IAudioClient2::SetClientProperties` con
     `AudioCategory_Communications` para activar el procesamiento de
     comunicaciones de Windows (incluye AEC/NS automático en mics que lo
     soporten).

2. **Selector de dispositivo de audio** (mic y parlantes)
   - Listar `IMMDeviceEnumerator::EnumAudioEndpoints`
   - Combos en la GUI; persistir en `%APPDATA%\fivecom\config.ini`

3. **Push-to-talk global** con hotkey configurable
   - `RegisterHotKey()` (funciona aunque la app no tenga foco) o low-level
     keyboard hook si se quiere capturar teclas que ya estén tomadas

4. **Indicador de nivel de audio** (VU meter) por peer en la GUI
   - Owner-draw en el ListBox o reemplazar por un control custom

5. **Volumen por peer** + slider de ganancia
   - Multiplicar PCM antes del mix; clip16 al final

6. **VAD real (Opus tiene uno) o webrtcvad**
   - Opus expone `OPUS_GET_IN_DTX` para saber si el último frame fue silencio

### Medianos (funcionalidad)

7. **NAT traversal** (UDP hole punching) para usar fuera de LAN
   - Necesita un servidor STUN público o uno propio mínimo
   - O un "rendezvous" server liviano que guíe el hole-punch entre clientes

8. **Múltiples canales** dentro de un Master
   - Cambiar `peers_` a `std::map<channel_id, std::vector<MasterPeer>>`
   - Paquetes nuevos: `PKT_CHANNEL_LIST`, `PKT_CHANNEL_JOIN`, `PKT_CHANNEL_LEAVE`

9. **Failover del Master** (que el "primero en conectar" se vuelva el master
   si el actual cae) — esto era parte del concepto original "P2P/bridge sin
   server fijo"
   - Heartbeat con timeout
   - Algoritmo de elección por menor peer_id activo
   - Migración de estado (lista de peers) — los peers ya la tienen vía
     PKT_PEER_LIST

10. **Cifrado** (DTLS/SRTP-like)
    - Opcional para uso doméstico, importante si se expone a internet
    - Curve25519 para handshake + ChaCha20-Poly1305 para audio (libsodium)

### Lejanos (polish y distribución)

11. **Instalador / autostart con Windows**
12. **Tray icon + minimizar a bandeja**
13. **Themes / dark mode** (Win32 con DWM)
14. **Métricas en la GUI**: latencia, packet loss, jitter, bitrate
15. **Soporte de overlay in-game** (más complejo, requiere hook de DirectX/GL)

---

## Referencias rápidas para futuras sesiones

- **Compilar**: `export PATH="/d/msys64/mingw64/bin:$PATH" && /d/msys64/usr/bin/make.exe`
- **Constantes clave**: [src/audio/wasapi.h:9-14](src/audio/wasapi.h#L9-L14) (SAMPLE_RATE=48000, FRAME_MS=20, FRAME_SAMPLES=960)
- **Bitrate Opus**: [src/audio/codec.h:11](src/audio/codec.h#L11) (`OPUS_BITRATE = 24000`)
- **Puerto UDP**: [src/network/packet.h:7](src/network/packet.h#L7) (`DEFAULT_PORT = 7777`)
- **Umbral VAD**: `detect_speech` en client.cpp y master.cpp (RMS > 500)
- **Timeout de peer**: [src/core/master.cpp:12](src/core/master.cpp#L12) (`PEER_TIMEOUT_MS = 10000`)
- **Jitter buffer**: target=2 frames (40ms), max=6 frames (120ms) — definidos en
  los constructores en master.h y client.h
