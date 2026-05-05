// FiveCom rendezvous worker.
//
// Endpoints (POST application/json):
//   /create   { public_ip, public_port, nick }            -> { code, secret }
//   /join     { code, public_ip, public_port, nick }      -> { master:{ip,port,nick} }
//             (efecto lateral: agrega el endpoint del cliente a la lista pending del master)
//   /poll     { code, secret }                            -> { pending:[{ip,port,nick}, ...] }
//             (devuelve y limpia la lista pending; lo llama el master cada ~1s)
//   /refresh  { code, secret, public_ip, public_port }    -> { ok:true }
//             (opcional, master refresca su endpoint si cambia el mapping NAT)
//
// Storage: KV namespace binding "FIVECOM", clave "room:<code>", TTL 1h.

const TTL_SECONDS = 30 * 24 * 3600; // 30 días — el master refresca cada 60s mientras corre,
                                    // y al cerrar la sala persiste 30d para poder reusar el mismo código.
const ALPHABET = 'ABCDEFGHJKLMNPQRSTUVWXYZ23456789'; // sin 0/O/1/I para evitar confusiones

function randomCode(len = 6) {
    const buf = new Uint8Array(len);
    crypto.getRandomValues(buf);
    let s = '';
    for (let i = 0; i < len; i++) s += ALPHABET[buf[i] % ALPHABET.length];
    return s;
}

function randomSecret() {
    const buf = new Uint8Array(16);
    crypto.getRandomValues(buf);
    return Array.from(buf).map(b => b.toString(16).padStart(2, '0')).join('');
}

function jsonResp(obj, status = 200) {
    return new Response(JSON.stringify(obj), {
        status,
        headers: {
            'content-type': 'application/json; charset=utf-8',
            'access-control-allow-origin': '*',
        },
    });
}

async function readJson(req) {
    try { return await req.json(); } catch { return null; }
}

function clipNick(s) { return (s || '').toString().slice(0, 32); }
function validIp(s)  { return typeof s === 'string' && /^[0-9.]{7,15}$/.test(s); }
function validPort(n){ return Number.isInteger(n) && n > 0 && n < 65536; }

export default {
    async fetch(req, env) {
        if (req.method === 'OPTIONS') {
            return new Response(null, {
                headers: {
                    'access-control-allow-origin': '*',
                    'access-control-allow-methods': 'POST, OPTIONS',
                    'access-control-allow-headers': 'content-type',
                },
            });
        }
        if (req.method !== 'POST') return jsonResp({ error: 'method_not_allowed' }, 405);

        const url = new URL(req.url);
        const body = await readJson(req);
        if (!body) return jsonResp({ error: 'bad_json' }, 400);

        if (url.pathname === '/create') {
            if (!validIp(body.public_ip) || !validPort(body.public_port))
                return jsonResp({ error: 'bad_fields' }, 400);

            let code = null;
            for (let i = 0; i < 6 && !code; i++) {
                const candidate = randomCode();
                const exists = await env.FIVECOM.get('room:' + candidate);
                if (!exists) code = candidate;
            }
            if (!code) return jsonResp({ error: 'busy' }, 503);

            const secret = randomSecret();
            const room = {
                master: { ip: body.public_ip, port: body.public_port, nick: clipNick(body.nick) },
                pending: [],
                secret,
                created: Date.now(),
            };
            await env.FIVECOM.put('room:' + code, JSON.stringify(room), { expirationTtl: TTL_SECONDS });
            return jsonResp({ code, secret });
        }

        if (url.pathname === '/join') {
            if (typeof body.code !== 'string' || !validIp(body.public_ip) || !validPort(body.public_port))
                return jsonResp({ error: 'bad_fields' }, 400);

            const key = 'room:' + body.code.toUpperCase();
            const raw = await env.FIVECOM.get(key);
            if (!raw) return jsonResp({ error: 'not_found' }, 404);
            const room = JSON.parse(raw);

            room.pending.push({
                ip: body.public_ip,
                port: body.public_port,
                nick: clipNick(body.nick),
                at: Date.now(),
            });
            if (room.pending.length > 32) room.pending = room.pending.slice(-32);

            await env.FIVECOM.put(key, JSON.stringify(room), { expirationTtl: TTL_SECONDS });
            return jsonResp({ master: room.master });
        }

        if (url.pathname === '/poll') {
            if (typeof body.code !== 'string' || typeof body.secret !== 'string')
                return jsonResp({ error: 'bad_fields' }, 400);

            const key = 'room:' + body.code.toUpperCase();
            const raw = await env.FIVECOM.get(key);
            if (!raw) return jsonResp({ error: 'not_found' }, 404);
            const room = JSON.parse(raw);
            if (room.secret !== body.secret) return jsonResp({ error: 'auth' }, 403);

            const pending = room.pending || [];
            // Sólo escribir KV si realmente había algo que limpiar (caso común
            // = lista vacía → 0 writes, así no rompemos el límite free de 1k/día).
            if (pending.length > 0) {
                room.pending = [];
                await env.FIVECOM.put(key, JSON.stringify(room), { expirationTtl: TTL_SECONDS });
            }
            return jsonResp({ pending });
        }

        if (url.pathname === '/refresh') {
            if (typeof body.code !== 'string' || typeof body.secret !== 'string')
                return jsonResp({ error: 'bad_fields' }, 400);

            const key = 'room:' + body.code.toUpperCase();
            const raw = await env.FIVECOM.get(key);
            if (!raw) return jsonResp({ error: 'not_found' }, 404);
            const room = JSON.parse(raw);
            if (room.secret !== body.secret) return jsonResp({ error: 'auth' }, 403);

            if (validIp(body.public_ip) && validPort(body.public_port)) {
                room.master.ip = body.public_ip;
                room.master.port = body.public_port;
            }
            if (typeof body.nick === 'string') {
                room.master.nick = clipNick(body.nick);
            }
            await env.FIVECOM.put(key, JSON.stringify(room), { expirationTtl: TTL_SECONDS });
            return jsonResp({ ok: true });
        }

        return jsonResp({ error: 'route' }, 404);
    },
};
