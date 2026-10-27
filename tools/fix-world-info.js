// Gives every mission in Fortify-Alpha's common/world_info.json its own missionGuid.
//
// The file ships with several missions sharing one made-up GUID ("24s9ag75-as67-..."), so world/validate, which
// looks missions up by GUID, returns the first of them (DeliverTheGoods) whichever was picked, e.g. for Close the
// Gate (MissionGen_1Gate, Stonewood tile 11). Each duplicated GUID is replaced by a stable one derived from the
// theater, tile and generator (32 hex digits, like Epic's IDs). Only those strings change; the rest of the file
// stays byte-for-byte as it was. The first run saves world_info.original.json next to it.
//
//   node fix-world-info.js            then restart the backend (it reads the file at startup)
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const kFile = path.resolve(__dirname, '../../Fortify-Alpha-main/common/world_info.json');
const kBackup = kFile.replace(/\.json$/, '.original.json');

const text = fs.readFileSync(kFile, 'utf8');
const world = JSON.parse(text);
const missions = world.missions.flatMap((t) => t.availableMissions.map((m) => ({ theaterId: t.theaterId, ...m })));

const uses = new Map();
for (const m of missions) uses.set(m.missionGuid, (uses.get(m.missionGuid) || 0) + 1);

const matches = [...text.matchAll(/"missionGuid": "([^"]*)"/g)];
if (matches.length !== missions.length) throw new Error(`expected ${missions.length} missionGuid strings, found ${matches.length}`);

let out = '';
let last = 0;
const changes = [];
matches.forEach((match, i) => {
    const m = missions[i];
    if (match[1] !== m.missionGuid) throw new Error(`missionGuid #${i} is out of order`);
    if (uses.get(m.missionGuid) < 2) return;
    const guid = crypto.createHash('md5').update(`${m.theaterId}:${m.tileIndex}:${m.missionGenerator}`).digest('hex').toUpperCase();
    out += text.slice(last, match.index) + `"missionGuid": "${guid}"`;
    last = match.index + match[0].length;
    changes.push(`${m.theaterId} tile ${String(m.tileIndex).padStart(3)}  ${m.missionGuid} -> ${guid}  ${m.missionGenerator.split('.').pop()}`);
});
out += text.slice(last);

if (!changes.length) {
    console.log('every missionGuid is already unique; nothing to do');
    return;
}
if (!fs.existsSync(kBackup)) fs.writeFileSync(kBackup, text);
fs.writeFileSync(kFile, out);
JSON.parse(out);  // still valid
console.log(`${changes.length} missions got their own GUID (backup: ${path.basename(kBackup)}):`);
for (const c of changes) console.log('  ' + c);
