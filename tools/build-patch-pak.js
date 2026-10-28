// Builds FortniteGame-WindowsClient_p.pak for OT6.5: the game's own DefaultEngine.ini (read from the installed
// main pak, so it always matches the build) with a few settings changed, so the client talks to the local
// backend directly instead of through Fiddler, and logs more of what the game does.
//
//   node build-patch-pak.js [backend host:port] [output pak]   (default 127.0.0.1:3551, into this game's Paks folder)
//
// For a co-op friend: node build-patch-pak.js <host's LAN or VPN address>:3551 friendFortniteGame-WindowsClient_p.pak
// and they copy that file into their own Paks folder.
//
// Play Limbo.bat uses two more forms:
//   node build-patch-pak.js --address <file>        prints the backend address in the file, cleaned up (a byte-order
//                                                   mark, spaces, http://; :3551 added), or 127.0.0.1:3551
//   node build-patch-pak.js --if-needed <host:port>  builds the pak only when it doesn't point at that backend yet
//
// OT6.5 only loads extra paks named like the main one with a _p suffix (MercuryLauncher found the same), and a
// file in a pak replaces the whole file, so the pak carries the complete ini.
const fs = require('fs');
const path = require('path');
const pak = require('./pak');

const kBuildDir = path.resolve(__dirname, '../../Fortnite (2252016-3240987)');
const kPaksDir = path.join(kBuildDir, 'FortniteGame/Content/Paks');
const kMainPak = path.join(kPaksDir, 'FortniteGame-WindowsClient.pak');
const ifNeeded = process.argv[2] === '--if-needed';
const args = ifNeeded ? process.argv.slice(3) : process.argv.slice(2);
const kPatchPak = args[1] ? path.resolve(args[1]) : path.join(kPaksDir, 'FortniteGame-WindowsClient_p.pak');
const kIniPath = 'FortniteGame/Config/DefaultEngine.ini';
const kConfigDir = path.resolve(__dirname, '../config');

function cleanAddress(text) {
    const address = String(text || '').replace(/^﻿/, '').trim().split(/\s+/)[0] || '';
    const bare = address.replace(/^[a-z]+:\/\//i, '').replace(/\/.*$/, '');
    if (!bare) return '127.0.0.1:3551';
    return /:\d+$/.test(bare) ? bare : `${bare}:3551`;
}

if (process.argv[2] === '--address') {
    let text = '';
    try { text = fs.readFileSync(process.argv[3], 'utf8'); } catch {}
    console.log(cleanAddress(text));
    process.exit(0);
}

const backend = cleanAddress(args[0]);

// The ini in a pak built before, or ''.
function builtIni() {
    try {
        const fd = fs.openSync(kPatchPak, 'r');
        const entry = pak.readIndex(fd).entries.find((e) => e.name === kIniPath);
        const text = entry ? pak.readEntry(fd, entry).toString('latin1') : '';
        fs.closeSync(fd);
        return text;
    } catch {
        return '';
    }
}

if (ifNeeded) {
    const ini = builtIni();
    if (ini.includes(`Domain="${backend}"`) && ini.includes(`ServerAddr="${backend.split(':')[0]}"`)) {
        console.log(`The patch pak points at ${backend}.`);
        process.exit(0);
    }
    console.log(`Building the patch pak for ${backend}...`);
}
const settings = [
    // every MCP service (account, fortnite, friends, ...) builds its URLs from this base
    ['OnlineSubsystemMcp.BaseServiceMcp', 'Protocol', '"http"'],
    ['OnlineSubsystemMcp.BaseServiceMcp', 'Domain', `"${backend}"`],
    ['/Script/Engine.NetworkSettings', 'n.VerifyPeer', 'false'],
    // chat and presence (XMPP): the backend's own server on 5222, no TLS. The client reads [OnlineSubsystemMcp.XMPP],
    // then the same section suffixed with its MCP config's name (-epicenv=Prod).
    ...['OnlineSubsystemMcp.XMPP', 'OnlineSubsystemMcp.XMPP Prod'].flatMap((section) => [
        [section, 'ServerAddr', `"${backend.split(':')[0]}"`],
        [section, 'ServerPort', '5222'],
        [section, 'bUseSSL', 'false'],
        [section, 'Domain', '"prod.ol.epicgames.com"'],
    ]),
    ['Core.Log', 'LogXmpp', 'verbose'],
    // what the server side of a mission does shows up under these
    ['Core.Log', 'LogOnline', 'verbose'],
    ['Core.Log', 'LogOnlineGame', 'verbose'],
    ['Core.Log', 'LogFort', 'verbose'],
];

// Sets key=value in [section], replacing the key's line or adding it under the header; adds the section if needed.
function setKey(lines, section, key, value) {
    const header = lines.findIndex((l) => l.trim() === `[${section}]`);
    if (header < 0) {
        lines.push('', `[${section}]`, `${key}=${value}`);
        return;
    }
    let end = header + 1;
    while (end < lines.length && !lines[end].startsWith('[')) end++;
    for (let i = header + 1; i < end; i++) {
        if (lines[i].split('=')[0].trim() === key) {
            lines[i] = `${key}=${value}`;
            return;
        }
    }
    lines.splice(header + 1, 0, `${key}=${value}`);
}

const fd = fs.openSync(kMainPak, 'r');
const entry = pak.readIndex(fd).entries.find((e) => e.name === kIniPath);
const original = pak.readEntry(fd, entry).toString('latin1');
fs.closeSync(fd);

const lines = original.split('\r\n');
for (const [section, key, value] of settings) setKey(lines, section, key, value);
const patched = Buffer.from(lines.join('\r\n'), 'latin1');

fs.mkdirSync(kConfigDir, { recursive: true });
fs.writeFileSync(path.join(kConfigDir, 'DefaultEngine.patched.ini'), patched);  // for reading; the pak is what counts
fs.mkdirSync(path.dirname(kPatchPak), { recursive: true });
pak.create(kPatchPak, [{ pakPath: kIniPath, data: patched }]);
console.log(`wrote ${kPatchPak}`);
if (!ifNeeded) for (const [section, key, value] of settings) console.log(`  [${section}] ${key}=${value}`);
