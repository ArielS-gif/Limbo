// Builds FortniteGame-WindowsClient_p.pak for OT6.5: the game's own DefaultEngine.ini (read from the installed
// main pak, so it always matches the build) with a few settings changed, so the client talks to the local
// backend directly instead of through Fiddler, and logs more of what the game does.
//
//   node build-patch-pak.js [backend host:port] [output pak]   (default 127.0.0.1:3551, into this game's Paks folder)
//
// For a co-op friend: node build-patch-pak.js <host's LAN or VPN address>:3551 friendFortniteGame-WindowsClient_p.pak
// and they copy that file into their own Paks folder.
//
// OT6.5 only loads extra paks named like the main one with a _p suffix (MercuryLauncher found the same), and a
// file in a pak replaces the whole file, so the pak carries the complete ini.
const fs = require('fs');
const path = require('path');
const pak = require('./pak');

const kBuildDir = path.resolve(__dirname, '../../Fortnite (2252016-3240987)');
const kPaksDir = path.join(kBuildDir, 'FortniteGame/Content/Paks');
const kMainPak = path.join(kPaksDir, 'FortniteGame-WindowsClient.pak');
const kPatchPak = process.argv[3] ? path.resolve(process.argv[3]) : path.join(kPaksDir, 'FortniteGame-WindowsClient_p.pak');
const kIniPath = 'FortniteGame/Config/DefaultEngine.ini';
const kConfigDir = path.resolve(__dirname, '../config');

const backend = process.argv[2] || '127.0.0.1:3551';
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
for (const [section, key, value] of settings) console.log(`  [${section}] ${key}=${value}`);
