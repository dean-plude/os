#!/usr/bin/env node
// An HTTP/2 test server for NovaOS's winhttp (userland/programs/httptest.c).
//
//   node tools/h2server.js <cert.pem> <key.pem> [https-port 8443] [http-port 8080]
//
// HTTPS (HTTP/2 by ALPN, HTTP/1.1 for clients that don't offer it):
//   /hello     a short page; X-Protocol says which HTTP version arrived
//   /big       300000 bytes, byte i = i * 7 % 251
//   /echo      POST: "echo: " + the body
//   /redirect  302 to /hello
// plain HTTP/1.1:
//   /chunked   three 1000-byte chunks (Transfer-Encoding: chunked)
//   /stream/BYTES/SEED  BYTES of a pattern (byte i = (i*7 + SEED*13 + (i>>16)) % 251),
//              written as fast as the client takes them (userland/programs/dltest.c)
// tools/selftest.py --suite network starts it with a throwaway certificate.
'use strict';
const crypto = require('crypto'), fs = require('fs'), http = require('http'), http2 = require('http2');

const [cert, key, httpsPort = 8443, httpPort = 8080] = process.argv.slice(2);
const log = (m) => console.log('[h2server] ' + m);

function handle(req, res) {
    log(`${req.method} ${req.url} HTTP/${req.httpVersion}`);
    const head = { 'x-protocol': req.httpVersion };
    if (req.url === '/conn') {
        let id = connections.get(req.socket);
        if (!id) connections.set(req.socket, id = nextConnection++);
        res.writeHead(200, { ...head, 'content-length': String(id).length });
        res.end(String(id));
    } else if (req.url === '/hello') {
        res.writeHead(200, { ...head, 'content-type': 'text/html' });
        res.end(`<html><body><h1>Hello over HTTP/${req.httpVersion === '2.0' ? '2' : req.httpVersion}</h1></body></html>\n`);
    } else if (req.url === '/big') {
        const b = Buffer.alloc(300000);
        for (let i = 0; i < b.length; i++) b[i] = i * 7 % 251;
        res.writeHead(200, { ...head, 'content-type': 'application/octet-stream', 'content-length': b.length });
        res.end(b);
    } else if (req.url === '/echo' && req.method === 'POST') {
        const parts = [];
        req.on('data', (d) => parts.push(d));
        req.on('end', () => {
            res.writeHead(200, { ...head, 'content-type': 'text/plain' });
            res.end('echo: ' + Buffer.concat(parts).toString());
        });
    } else if (req.url === '/redirect') {
        res.writeHead(302, { ...head, location: '/hello' });
        res.end();
    } else if (req.url === '/chunked') {
        res.writeHead(200, { ...head, 'content-type': 'text/plain' });   // no length: chunked
        let i = 0;
        const next = () => {
            if (i === 3) return res.end();
            res.write(`chunk ${i++} `.padEnd(1000, '.'));
            setTimeout(next, 20);
        };
        next();
    } else if (/^\/stream\/\d+\/\d+$/.test(req.url)) {
        const [, bytes, seed] = req.url.split('/').slice(1).map(Number);
        res.writeHead(200, { ...head, 'content-type': 'application/octet-stream', 'content-length': bytes });
        const block = 65536;
        let sent = 0;
        const more = () => {
            while (sent < bytes) {
                const b = Buffer.alloc(Math.min(block, bytes - sent));
                for (let i = 0; i < b.length; i++) {
                    const k = sent + i;
                    b[i] = (k * 7 + seed * 13 + Math.floor(k / 65536)) % 251;
                }
                sent += b.length;
                if (!res.write(b)) return res.once('drain', more);
            }
            res.end();
        };
        more();
    } else {
        res.writeHead(404, { ...head, 'content-type': 'text/plain' });
        res.end('not found\n');
    }
}

const connections = new WeakMap();
let nextConnection = 1;
function websocket(req, socket, head) {
    const key = req.headers['sec-websocket-key'];
    if (req.url !== '/websocket' || !key) return socket.destroy();
    const accept = crypto.createHash('sha1').update(key + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11').digest('base64');
    socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' +
                 `Sec-WebSocket-Accept: ${accept}\r\n\r\n`);
    let pending = head;
    socket.on('data', (chunk) => {
        pending = Buffer.concat([pending, chunk]);
        while (pending.length >= 2) {
            const opcode = pending[0] & 0x0f, masked = pending[1] & 0x80;
            let length = pending[1] & 0x7f, offset = 2;
            if (length === 126) {
                if (pending.length < 4) return;
                length = pending.readUInt16BE(2); offset = 4;
            } else if (length === 127) {
                if (pending.length < 10) return;
                const wide = pending.readBigUInt64BE(2);
                if (wide > BigInt(Number.MAX_SAFE_INTEGER)) return socket.destroy();
                length = Number(wide); offset = 10;
            }
            if (!masked) return socket.destroy();
            if (pending.length < offset + 4 + length) return;
            const mask = pending.subarray(offset, offset + 4);
            const data = Buffer.from(pending.subarray(offset + 4, offset + 4 + length));
            for (let i = 0; i < data.length; i++) data[i] ^= mask[i & 3];
            pending = pending.subarray(offset + 4 + length);
            if (opcode === 1 || opcode === 2) {
                const frame = Buffer.alloc(data.length + 2);
                frame[0] = 0x80 | opcode;
                frame[1] = data.length;
                data.copy(frame, 2);
                socket.write(frame);
            } else if (opcode === 8) {
                socket.write(Buffer.from([0x88, data.length, ...data]));
                socket.end();
                return;
            } else if (opcode === 9) {
                socket.write(Buffer.from([0x8a, data.length, ...data]));
            }
        }
    });
}

http2.createSecureServer({ cert: fs.readFileSync(cert), key: fs.readFileSync(key), allowHTTP1: true }, handle)
    .listen(+httpsPort, '127.0.0.1', () => log(`HTTPS (h2, http/1.1) on 127.0.0.1:${httpsPort}`));
const httpServer = http.createServer(handle);
httpServer.on('upgrade', websocket);
httpServer.listen(+httpPort, '127.0.0.1', () => log(`HTTP on 127.0.0.1:${httpPort}`));
