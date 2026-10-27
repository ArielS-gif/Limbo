// Runs the Fortify-Alpha backend unchanged and logs every request it answers: method, URL, status, the JSON
// request body and the start of the response body. Secrets (passwords, tokens, exchange codes) are blanked.
//
//   node backend-trace.js <Fortify-Alpha folder> <log file>
const fs = require('fs');
const path = require('path');
const http = require('http');

const backendDir = path.resolve(process.argv[2]);
const log = fs.createWriteStream(path.resolve(process.argv[3]), { flags: 'w' });
const kMaxBody = 6000;
const kSecret = /^(password|access_token|refresh_token|exchange_code|code|token|client_secret)$/i;

function redact(value) {
    if (Array.isArray(value)) return value.map(redact);
    if (value && typeof value === 'object') {
        const out = {};
        for (const [k, v] of Object.entries(value)) out[k] = kSecret.test(k) ? '<hidden>' : redact(v);
        return out;
    }
    return value;
}

function clip(text) {
    return text.length > kMaxBody ? text.slice(0, kMaxBody) + ` ... (${text.length} chars)` : text;
}

function describeResponse(res) {
    const chunks = res._traceChunks || [];
    if (!chunks.length) return '';
    const body = Buffer.concat(chunks);
    const type = String(res.getHeader('content-type') || '');
    if (!/json|text/.test(type) && body.length) return `<${body.length} bytes ${type || 'binary'}>`;
    const text = body.toString('utf8');
    try { return clip(JSON.stringify(redact(JSON.parse(text)))); } catch { return clip(text); }
}

function capture(res, chunk) {
    if (chunk == null || typeof chunk === 'function') return;
    (res._traceChunks ||= []).push(Buffer.isBuffer(chunk) ? chunk : Buffer.from(String(chunk)));
}

const write = http.ServerResponse.prototype.write;
const end = http.ServerResponse.prototype.end;
http.ServerResponse.prototype.write = function (chunk, ...rest) {
    capture(this, chunk);
    return write.call(this, chunk, ...rest);
};
http.ServerResponse.prototype.end = function (chunk, ...rest) {
    capture(this, chunk);
    const req = this.req;
    if (req) {
        const body = req.body && Object.keys(req.body).length ? clip(JSON.stringify(redact(req.body))) : '';
        log.write(`[${new Date().toISOString()}] ${req.method} ${req.url} -> ${this.statusCode}\n`);
        if (body) log.write(`  request:  ${body}\n`);
        const response = describeResponse(this);
        if (response) log.write(`  response: ${response}\n`);
    }
    return end.call(this, chunk, ...rest);
};

process.chdir(backendDir);  // dotenv-safe reads .env from the working directory
require(path.join(backendDir, 'index.js'));
