# FiveCom Voice Chat

Aplicación nativa Windows para chat de voz P2P con audio WASAPI, codec Opus y rendezvous por Cloudflare Worker.

## Build

Requiere MSYS2/MinGW64 y Opus instalado.

```bash
D:/msys64/usr/bin/make.exe
```

Genera `fivecom-voice-chat.exe`.

## Uso

El binario tiene una URL de rendezvous embebida. Opcionalmente se puede poner `rendezvous_url.txt` al lado del `.exe` para sobreescribirla sin recompilar.
