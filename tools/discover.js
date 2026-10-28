// Finds Limbo backends on the networks this PC is on, for Play Limbo.bat: a "LIMBO?" broadcast to UDP 3551 on each
// one (a LAN, Radmin VPN's 26.x; the backend's src/discovery.js answers) and the answers for a moment. Prints one line
// per backend found, the ones with players online first:
//   <address>:<port> <PC name>: <players online, or "nobody playing">
// This PC's own backend is left out (LIMBO_DISCOVER_SELF=1 keeps it, for tests on one PC).
//
//   node discover.js
const dgram = require('dgram');
const os = require('os');

const kPort = 3551, kWaitMs = 1500;

const own = new Set(['127.0.0.1']);
const targets = new Set(['255.255.255.255']);
for (const list of Object.values(os.networkInterfaces())) {
    for (const a of list) {
        if (a.family !== 'IPv4' || a.internal) continue;
        own.add(a.address);
        // The network's own broadcast address: Windows sends 255.255.255.255 out of one network only.
        const ip = a.address.split('.').map(Number), mask = a.netmask.split('.').map(Number);
        targets.add(ip.map((b, i) => (b & mask[i]) | (~mask[i] & 255)).join('.'));
    }
}

const found = new Map();
const socket = dgram.createSocket('udp4');
socket.on('message', (message, from) => {
    try {
        const answer = JSON.parse(message.toString());
        if (answer.limbo && (process.env.LIMBO_DISCOVER_SELF || !own.has(from.address))) found.set(from.address, answer);
    } catch {}
});
socket.on('error', () => {});
socket.bind(() => {
    socket.setBroadcast(true);
    for (const target of targets) socket.send('LIMBO?', kPort, target, () => {});
    setTimeout(() => {
        // Only plain characters: the batch file echoes the line (& | < > ( ) would be read as commands).
        const plain = (text) => String(text).replace(/[^\w .,'-]/g, '');
        const backends = [...found].sort(([, a], [, b]) => (b.online || []).length - (a.online || []).length);
        for (const [address, a] of backends) {
            const online = (a.online || []).map(plain);
            console.log(`${address}:${Number(a.port) || kPort} ${plain(a.name)}: ${online.length ? online.join(', ') : 'nobody playing'}`);
        }
        socket.close();
    }, kWaitMs);
});
