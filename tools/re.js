// Static analysis helper for the OT6.5 client exe (32-bit, image base 0x400000 in the file; it loads at another
// base at runtime, so always talk in file VAs here: VA = RVA + 0x400000). Disassembly comes from dumpbin; its output
// is annotated with strings, native UFunction names (out\re\natives.txt from the `natives` trace command), class
// vtables (out\re\classes.txt) and a few known globals.
//
//   node re.js strings <regex> [max]      strings (ASCII and UTF-16) matching regex, with their VA
//   node re.js strref <regex> [max]       code referencing strings that match regex, with the containing function
//   node re.js xref <VA>                  references to VA: absolute (any section) and call/jmp rel32
//   node re.js func <VA | native-regex>   disassemble the whole function containing VA (or that native's entry)
//   node re.js disasm <VA> [bytes]        disassemble a range (default 0x80 bytes)
//   node re.js callers <VA>               call sites of the function containing VA, with their functions
//   node re.js vtable <class-regex> [n]   the first n (default 120) entries of a class's vtable
//   node re.js natives <regex>            native UFunctions whose path matches regex
//   node re.js classes <regex>            classes matching regex: vtable, size, parent
//
// VAs are hex, with or without 0x. Function bounds come from a table of function starts (call targets, vtable
// entries and natives that sit after padding or on a 16-byte boundary), cached in tools\.cache.
const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

const kExe = path.resolve(__dirname, '../../Fortnite (2252016-3240987)/FortniteGame/Binaries/Win32/FortniteClient-Win64-Shipping.exe');
const kDumpbin = 'C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\VC\\Tools\\MSVC\\14.51.36231\\bin\\Hostx64\\x86\\dumpbin.exe';
const kReDir = path.resolve(__dirname, '../out/re');
const kCacheDir = path.resolve(__dirname, '.cache');
const kMaxFunctionBytes = 0x6000;

// ---- the exe -----------------------------------------------------------------------------------------------------

const exe = fs.readFileSync(kExe);
const peOff = exe.readInt32LE(0x3c);
const imageBase = exe.readUInt32LE(peOff + 24 + 28);
const sections = [];
{
    const count = exe.readUInt16LE(peOff + 6);
    const first = peOff + 24 + exe.readUInt16LE(peOff + 20);
    for (let i = 0; i < count; i++) {
        const s = first + i * 40;
        sections.push({
            name: exe.toString('latin1', s, s + 8).replace(/\0+$/, ''),
            va: imageBase + exe.readUInt32LE(s + 12),
            vsize: exe.readUInt32LE(s + 8),
            raw: exe.readUInt32LE(s + 20),
            rawSize: exe.readUInt32LE(s + 16),
        });
    }
}
const text = sections.find((s) => s.name === '.text');
const sectionOf = (va) => sections.find((s) => va >= s.va && va < s.va + Math.max(s.vsize, s.rawSize));
const fileOffset = (va) => {
    const s = sectionOf(va);
    return s && va - s.va < s.rawSize ? s.raw + (va - s.va) : -1;
};
const vaOf = (s, off) => s.va + (off - s.raw);
const inText = (va) => va >= text.va && va < text.va + text.vsize;
const hex = (n) => n.toString(16).toUpperCase().padStart(8, '0');
const parseVA = (s) => parseInt(String(s).replace(/^0x/i, '').replace(/h$/i, ''), 16);

// ---- names: natives, vtables, known globals ----------------------------------------------------------------------

const names = new Map();     // VA -> label
const natives = [];          // { va, flags, path }
const classes = [];          // { vtable, size, parent, path }
function loadTable(file, onRow) {
    const p = path.join(kReDir, file);
    if (!fs.existsSync(p)) return;
    for (const line of fs.readFileSync(p, 'latin1').split('\n')) {
        if (!line || line.startsWith('#')) continue;
        onRow(line.replace(/\r$/, '').split('\t'));
    }
}
loadTable('natives.txt', ([rva, flags, p]) => {
    const va = parseInt(rva, 16) ? parseInt(rva, 16) + imageBase : 0;
    natives.push({ va, flags: parseInt(flags, 16), path: p });
});
{
    // Identical code is folded by the linker, so several natives can share one entry (e.g. cheats compiled out
    // to the same empty body); say so instead of naming just one of them.
    const byVa = new Map();
    for (const n of natives) if (n.va) byVa.set(n.va, [...(byVa.get(n.va) || []), n.path]);
    for (const [va, paths] of byVa) {
        if (paths.length > 100) names.set(va, `UObject::ProcessInternal (blueprint script, ${paths.length} functions)`);
        else names.set(va, paths.length > 1 ? `native ${paths[0]} (+${paths.length - 1} more share this code)` : `native ${paths[0]}`);
    }
}
loadTable('classes.txt', ([rva, size, parent, p]) => {
    const vtable = parseInt(rva, 16) ? parseInt(rva, 16) + imageBase : 0;
    classes.push({ vtable, size: parseInt(size, 16), parent, path: p });
    if (vtable && !names.has(vtable)) names.set(vtable, `vtable ${p}`);
});
for (const [rva, label] of [[0x2DC3AAC, 'GObjects (FUObjectArray.ObjObjects)'], [0x2D87358, 'GNames (TNameEntryArray*)'],
                            [0x2E68BEC, 'GWorld'], [0x9A9E70, 'UObject::ProcessEvent']])
    names.set(rva + imageBase, label);

// ---- strings -------------------------------------------------------------------------------------------------------

// The string starting exactly at VA (ASCII or UTF-16, at least 3 printable characters), or null.
function stringAt(va) {
    const off = fileOffset(va);
    if (off < 0) return null;
    const printable = (c) => (c >= 0x20 && c < 0x7f) || c === 9 || c === 10 || c === 13;
    let ascii = '';
    for (let i = off; i < exe.length && printable(exe[i]) && ascii.length < 300; i++) ascii += String.fromCharCode(exe[i]);
    if (ascii.length >= 3 && exe[off + ascii.length] === 0) return JSON.stringify(ascii);
    let wide = '';
    for (let i = off; i + 1 < exe.length && exe[i + 1] === 0 && printable(exe[i]) && wide.length < 300; i += 2) wide += String.fromCharCode(exe[i]);
    if (wide.length >= 3 && exe.readUInt16LE(off + wide.length * 2) === 0) return 'L' + JSON.stringify(wide);
    return null;
}

function findStrings(re, max) {
    const out = [];
    for (const s of sections.filter((x) => x.name === '.rdata' || x.name === '.data')) {
        const end = s.raw + s.rawSize;
        for (let i = s.raw; i < end && out.length < max; i++) {
            // ASCII run starting here (previous byte is not printable)
            const c = exe[i];
            if (c >= 0x20 && c < 0x7f && (i === s.raw || exe[i - 1] < 0x20 || exe[i - 1] >= 0x7f)) {
                let j = i;
                while (j < end && exe[j] >= 0x20 && exe[j] < 0x7f) j++;
                if (j - i >= 4 && exe[j] === 0) {
                    const str = exe.toString('latin1', i, j);
                    if (re.test(str)) out.push({ va: vaOf(s, i), enc: 'ascii', str });
                }
            }
            // UTF-16 run starting here (even offset, previous code unit not printable)
            if ((i - s.raw) % 2 === 0 && c >= 0x20 && c < 0x7f && exe[i + 1] === 0 &&
                (i === s.raw || !(exe[i - 1] === 0 && exe[i - 2] >= 0x20 && exe[i - 2] < 0x7f))) {
                let j = i;
                while (j + 1 < end && exe[j] >= 0x20 && exe[j] < 0x7f && exe[j + 1] === 0) j += 2;
                if ((j - i) / 2 >= 4 && exe[j] === 0 && exe[j + 1] === 0) {
                    const str = exe.toString('utf16le', i, j);
                    if (re.test(str)) out.push({ va: vaOf(s, i), enc: 'utf16', str });
                }
            }
        }
    }
    return out;
}

// ---- function starts ------------------------------------------------------------------------------------------------

function functionStarts() {
    const cache = path.join(kCacheDir, 'funcstarts.bin');
    const stamp = `${exe.length}-${fs.statSync(kExe).mtimeMs}-${natives.length}`;
    const stampFile = cache + '.stamp';
    if (fs.existsSync(cache) && fs.existsSync(stampFile) && fs.readFileSync(stampFile, 'latin1') === stamp)
        return new Uint32Array(fs.readFileSync(cache).buffer.slice(0));
    const plausible = (va) => {
        if (!inText(va)) return false;
        const off = fileOffset(va);
        const before = exe[off - 1];
        return va % 16 === 0 || before === 0xcc || before === 0xc3 || before === 0x90;
    };
    const set = new Set();
    const tEnd = text.raw + text.rawSize;
    for (let i = text.raw; i + 5 <= tEnd; i++) {
        if (exe[i] !== 0xe8) continue;
        const target = vaOf(text, i) + 5 + exe.readInt32LE(i + 1);
        if (plausible(target)) set.add(target >>> 0);
    }
    for (const s of sections.filter((x) => x.name === '.rdata')) {
        for (let i = s.raw; i + 4 <= s.raw + s.rawSize; i += 4) {
            const v = exe.readUInt32LE(i);
            if (plausible(v)) set.add(v);
        }
    }
    for (const n of natives) if (n.va && inText(n.va)) set.add(n.va);
    const arr = Uint32Array.from([...set].sort((a, b) => a - b));
    fs.mkdirSync(kCacheDir, { recursive: true });
    fs.writeFileSync(cache, Buffer.from(arr.buffer));
    fs.writeFileSync(stampFile, stamp);
    return arr;
}
let starts = null;
function functionBounds(va) {
    starts ??= functionStarts();
    let lo = 0, hi = starts.length - 1, best = -1;
    while (lo <= hi) {
        const mid = (lo + hi) >> 1;
        if (starts[mid] <= va) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    if (best < 0) return null;
    const start = starts[best];
    const next = best + 1 < starts.length ? starts[best + 1] : text.va + text.vsize;
    return { start, end: Math.min(next, start + kMaxFunctionBytes) };
}
const describeFunction = (va) => {
    const b = functionBounds(va);
    if (!b) return '?';
    return `${hex(b.start)}${names.has(b.start) ? ` (${names.get(b.start)})` : ''}`;
};

// ---- disassembly ----------------------------------------------------------------------------------------------------

function annotate(line) {
    const notes = [];
    for (const m of line.matchAll(/\b([0-9A-F]{8})h?\b/g)) {
        const va = parseInt(m[1], 16);
        if (va < imageBase || !sectionOf(va)) continue;
        if (names.has(va)) notes.push(names.get(va));
        else {
            const s = stringAt(va);
            if (s) notes.push(s.length > 120 ? s.slice(0, 120) + '..."' : s);
        }
    }
    return notes.length ? `${line}    ; ${notes.join(' | ')}` : line;
}

function disasm(start, end) {
    const out = execFileSync(kDumpbin, ['/nologo', '/disasm', `/range:0x${hex(start)},0x${hex(end - 1)}`, kExe],
                             { encoding: 'latin1', maxBuffer: 256 * 1024 * 1024 });
    return out.split(/\r?\n/).filter((l) => /^\s+[0-9A-F]{8}:/.test(l)).map(annotate).join('\n');
}

// ---- references -------------------------------------------------------------------------------------------------------

function absoluteRefs(va) {
    const out = [];
    const needle = Buffer.alloc(4);
    needle.writeUInt32LE(va >>> 0);
    for (const s of sections) {
        let i = exe.indexOf(needle, s.raw);
        while (i >= 0 && i < s.raw + s.rawSize) {
            out.push({ at: vaOf(s, i), section: s.name });
            i = exe.indexOf(needle, i + 1);
        }
    }
    return out;
}

function relativeRefs(va) {
    const out = [];
    const tEnd = text.raw + text.rawSize;
    for (let i = text.raw; i + 5 <= tEnd; i++) {
        const op = exe[i];
        if (op !== 0xe8 && op !== 0xe9) continue;
        if ((vaOf(text, i) + 5 + exe.readInt32LE(i + 1)) >>> 0 === va) out.push({ at: vaOf(text, i), kind: op === 0xe8 ? 'call' : 'jmp' });
    }
    // jcc rel32 (0F 8x)
    for (let i = text.raw; i + 6 <= tEnd; i++) {
        if (exe[i] !== 0x0f || (exe[i + 1] & 0xf0) !== 0x80) continue;
        if ((vaOf(text, i) + 6 + exe.readInt32LE(i + 2)) >>> 0 === va) out.push({ at: vaOf(text, i), kind: 'jcc' });
    }
    return out;
}

// The instruction containing `at`: disassemble a little before it and keep the line whose range covers `at`.
function instructionAt(at) {
    const b = functionBounds(at);
    const from = Math.max(b ? b.start : at - 16, at - 16);
    const lines = disasm(from, at + 16).split('\n');
    let found = null;
    for (const l of lines) {
        const m = l.match(/^\s+([0-9A-F]{8}):/);
        if (m && parseInt(m[1], 16) <= at) found = l;
    }
    return found ? found.trim() : '(not on an instruction boundary)';
}

// ---- commands ----------------------------------------------------------------------------------------------------------

function resolveTarget(arg) {
    if (/^(0x)?[0-9a-f]{6,8}h?$/i.test(arg)) return parseVA(arg);
    const re = new RegExp(arg);
    const hits = natives.filter((n) => n.va && re.test(n.path));
    if (hits.length !== 1) {
        console.log(hits.length ? `ambiguous, ${hits.length} natives match:` : 'no native matches');
        for (const h of hits.slice(0, 20)) console.log(`  ${hex(h.va)}  ${h.path}`);
        process.exit(1);
    }
    console.log(`${hits[0].path} -> ${hex(hits[0].va)}`);
    return hits[0].va;
}

const [cmd, arg, arg2] = process.argv.slice(2);
switch (cmd) {
case 'strings': {
    for (const s of findStrings(new RegExp(arg), Number(arg2) || 200)) console.log(`${hex(s.va)}  ${s.enc.padEnd(5)}  ${s.str}`);
    break;
}
case 'strref': {
    for (const s of findStrings(new RegExp(arg), Number(arg2) || 40)) {
        console.log(`${hex(s.va)}  ${s.enc}  ${JSON.stringify(s.str)}`);
        for (const r of absoluteRefs(s.va)) {
            if (r.section === '.text') console.log(`    ${hex(r.at)}  in ${describeFunction(r.at)}  ${instructionAt(r.at)}`);
            else console.log(`    ${hex(r.at)}  ${r.section} (pointer)`);
        }
    }
    break;
}
case 'xref': {
    const va = parseVA(arg);
    console.log(`references to ${hex(va)}${names.has(va) ? ` (${names.get(va)})` : ''}:`);
    for (const r of absoluteRefs(va)) {
        if (r.section === '.text') console.log(`  ${hex(r.at)}  abs   in ${describeFunction(r.at)}  ${instructionAt(r.at)}`);
        else console.log(`  ${hex(r.at)}  ${r.section} (pointer)${names.has(r.at) ? ` ${names.get(r.at)}` : ''}`);
    }
    for (const r of relativeRefs(va)) console.log(`  ${hex(r.at)}  ${r.kind.padEnd(5)} in ${describeFunction(r.at)}`);
    break;
}
case 'func': {
    const va = resolveTarget(arg);
    const b = functionBounds(va);
    console.log(`function ${hex(b.start)}..${hex(b.end)}${names.has(b.start) ? ` (${names.get(b.start)})` : ''}${b.end - b.start >= kMaxFunctionBytes ? ' (truncated)' : ''}`);
    console.log(disasm(b.start, b.end));
    break;
}
case 'disasm': {
    const va = parseVA(arg);
    console.log(disasm(va, va + (arg2 ? parseVA(arg2) : 0x80)));
    break;
}
case 'callers': {
    const b = functionBounds(parseVA(arg));
    console.log(`callers of ${describeFunction(b.start)}:`);
    for (const r of relativeRefs(b.start)) console.log(`  ${hex(r.at)}  ${r.kind.padEnd(5)} in ${describeFunction(r.at)}`);
    for (const r of absoluteRefs(b.start)) console.log(`  ${hex(r.at)}  pointer in ${r.section}${r.section === '.text' ? ` in ${describeFunction(r.at)}` : names.has(r.at) ? ` ${names.get(r.at)}` : ''}`);
    break;
}
case 'vtable': {
    const re = new RegExp(arg);
    for (const c of classes.filter((x) => re.test(x.path)).slice(0, 3)) {
        console.log(`${c.path}  vtable ${hex(c.vtable)}  size 0x${c.size.toString(16)}  parent ${c.parent}`);
        const off = fileOffset(c.vtable);
        for (let i = 0; i < (Number(arg2) || 120); i++) {
            const f = exe.readUInt32LE(off + i * 4);
            if (!inText(f)) break;
            console.log(`  [${String(i).padStart(3)}] +0x${(i * 4).toString(16).padStart(3, '0')}  ${hex(f)}${names.has(f) ? `  ${names.get(f)}` : ''}`);
        }
    }
    break;
}
case 'natives': {
    const re = new RegExp(arg);
    for (const n of natives.filter((x) => re.test(x.path))) console.log(`${n.va ? hex(n.va) : '--------'}  ${hex(n.flags)}  ${n.path}`);
    break;
}
case 'classes': {
    const re = new RegExp(arg);
    for (const c of classes.filter((x) => re.test(x.path))) console.log(`${c.vtable ? hex(c.vtable) : '--------'}  0x${c.size.toString(16).padStart(4, '0')}  ${c.parent.padEnd(28)} ${c.path}`);
    break;
}
default:
    console.log(fs.readFileSync(__filename, 'utf8').split('\n').filter((l) => l.startsWith('//')).join('\n'));
    process.exitCode = 1;
}
