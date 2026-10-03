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
// tools/selftest.py --suite network starts it with a throwaway certificate.
'use strict';
const fs = require('fs'), http = require('http'), http2 = require('http2');

const [cert, key, httpsPort = 8443, httpPort = 8080] = process.argv.slice(2);
const log = (m) => console.log('[h2server] ' + m);

function handle(req, res) {
    log(`${req.method} ${req.url} HTTP/${req.httpVersion}`);
    const head = { 'x-protocol': req.httpVersion };
    if (req.url === '/hello') {
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
    } else {
        res.writeHead(404, { ...head, 'content-type': 'text/plain' });
        res.end('not found\n');
    }
}

http2.createSecureServer({ cert: fs.readFileSync(cert), key: fs.readFileSync(key), allowHTTP1: true }, handle)
    .listen(+httpsPort, '127.0.0.1', () => log(`HTTPS (h2, http/1.1) on 127.0.0.1:${httpsPort}`));
http.createServer(handle).listen(+httpPort, '127.0.0.1', () => log(`HTTP on 127.0.0.1:${httpPort}`));
