# FiveCom rendezvous (Cloudflare Worker)

Server gratis (plan free) que sólo intercambia endpoints públicos para que los
peers se "punchen" entre sí. **El audio nunca pasa por acá** — va P2P directo.

## Costo y límites

Plan free de Cloudflare Workers: 100k requests/día. Una sesión típica usa
~3-100 requests (depende del tiempo que esté abierta la sala, el master pollea
cada 1s). Sobra muchísimo para uso personal. KV gratis hasta 1k writes/día.

## Pre-requisito

Tener Node.js instalado (trae `npm`). Si no lo tenés:
descargar el instalador LTS de https://nodejs.org → siguiente, siguiente, siguiente.
Verificar abriendo una **PowerShell nueva** y corriendo `node --version`.

## Deploy (una sola vez)

**Todos los comandos de abajo se corren desde esta misma carpeta:**
`D:\Coder\fivecom\worker\`

Abrí PowerShell **acá** (o `cd D:\Coder\fivecom\worker` desde donde estés).

1. Crear cuenta gratis en https://cloudflare.com (no pide tarjeta).

2. Instalar wrangler (CLI de Cloudflare). **Esto sí es global, no importa
   desde dónde lo corras** — instala el comando `wrangler` en tu PATH:
   ```
   npm install -g wrangler
   ```

3. Login (abre el browser y autoriza la cuenta de Cloudflare):
   ```
   wrangler login
   ```

4. Crear el namespace KV (esto sí, **desde `D:\Coder\fivecom\worker\`**, porque
   wrangler lee el `wrangler.toml` de la carpeta actual):
   ```
   wrangler kv namespace create FIVECOM
   ```
   Te devuelve algo como:
   ```
   [[kv_namespaces]]
   binding = "FIVECOM"
   id = "abc123def456..."
   ```
   Copiá el `id` y pegalo en `wrangler.toml` (reemplazando
   `REEMPLAZAR_CON_ID_DEL_NAMESPACE`). El archivo queda así:
   ```
   kv_namespaces = [
     { binding = "FIVECOM", id = "abc123def456..." }
   ]
   ```

5. Deploy (también desde `D:\Coder\fivecom\worker\`):
   ```
   wrangler deploy
   ```
   Al final imprime la URL, algo como:
   ```
   https://fivecom-rendezvous.pibecom.workers.dev
   ```

6. En la PC donde corre `fivecom.exe` (puede ser otra), crear al lado del .exe
   (o sea en `D:\Coder\fivecom\rendezvous_url.txt`) un archivo de texto con
   esa URL en una sola línea:
   ```
   https://fivecom-rendezvous.pibecom.workers.dev
   ```

¡Listo! Ya pueden crear y unirse a canales por internet usando un código corto.

## Cambiar de servidor / cuenta

Si en el futuro querés cambiar el server, sólo cambiás el contenido de
`rendezvous_url.txt`. No hace falta recompilar el .exe.
