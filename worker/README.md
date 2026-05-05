# FiveCom Rendezvous Worker

Cloudflare Worker usado como rendezvous para FiveCom. Su única función es
intercambiar endpoints públicos entre peers para ayudar al UDP hole punching.
El audio no pasa por el Worker: el audio viaja P2P por UDP.

## Requisitos

- Cuenta de Cloudflare.
- Node.js instalado.
- `wrangler` instalado globalmente:

```bash
npm install -g wrangler
```

## Configuración

Iniciar sesión en Cloudflare:

```bash
wrangler login
```

Crear el namespace KV:

```bash
wrangler kv namespace create FIVECOM
```

Cloudflare devuelve un bloque con un `id`. Reemplazar el placeholder en
`wrangler.toml`:

```toml
kv_namespaces = [
  { binding = "FIVECOM", id = "REPLACE_WITH_YOUR_KV_NAMESPACE_ID" }
]
```

No commitear IDs reales de Cloudflare si el repositorio va a compartirse. Para
uso local, podés mantener una copia ignorada por Git, por ejemplo
`wrangler.local.toml`.

## Deploy

Desde esta carpeta:

```bash
wrangler deploy
```

El deploy imprime una URL del Worker. FiveCom puede usar una URL embebida en el
binario o un archivo opcional `rendezvous_url.txt` al lado del `.exe` para
sobreescribirla sin recompilar.

## Endpoints

- `POST /create`: crea o refresca un canal.
- `POST /join`: registra un cliente para que el master lo descubra.
- `POST /poll`: el master consulta clientes pendientes.
- `POST /refresh`: mantiene vivo el canal.
