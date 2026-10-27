// Reads and writes UE 4.12 pak files (version 3), enough for config overrides.
//
//   node pak.js list <pak>
//   node pak.js extract <pak> <path in pak> <output file>
//   node pak.js create <output pak> <path in pak>=<local file> [...]
//
// Layout (version 3): each file's data is preceded by a copy of its entry header (offset field 0); the index is
// the mount point, the entry count and (filename, entry) pairs; the 44-byte footer is magic, version, index
// offset and size, and the SHA-1 of the index. `create` writes uncompressed entries, like Mercury's patch pak.
const crypto = require('crypto');
const fs = require('fs');
const zlib = require('zlib');

const kMagic = 0x5A6F12E1;
const kVersion = 3;
const kFooterSize = 44;
const kMountPoint = '../../../';

const sha1 = (buf) => crypto.createHash('sha1').update(buf).digest();

function writeFString(parts, text) {
    const len = Buffer.alloc(4);
    len.writeInt32LE(text.length + 1);
    parts.push(len, Buffer.from(text + '\0', 'latin1'));
}

function entryHeader(offset, size, hash) {
    const b = Buffer.alloc(53);
    b.writeBigInt64LE(BigInt(offset), 0);
    b.writeBigInt64LE(BigInt(size), 8);   // Size
    b.writeBigInt64LE(BigInt(size), 16);  // UncompressedSize
    b.writeInt32LE(0, 24);                // CompressionMethod: none
    hash.copy(b, 28);
    b.writeUInt8(0, 48);                  // bEncrypted
    b.writeUInt32LE(0, 49);               // CompressionBlockSize
    return b;
}

// Reads `length` bytes at `position` of an open file (paks can be far bigger than memory, so never whole).
function readAt(fd, position, length) {
    const buf = Buffer.alloc(length);
    let done = 0;
    while (done < length) {
        const n = fs.readSync(fd, buf, done, length - done, position + done);
        if (n === 0) throw new Error('unexpected end of file');
        done += n;
    }
    return buf;
}

function readIndex(fd) {
    const fileSize = fs.fstatSync(fd).size;
    const footer = readAt(fd, fileSize - kFooterSize, kFooterSize);
    if (footer.readUInt32LE(0) !== kMagic) throw new Error('not a pak file (bad magic)');
    const version = footer.readInt32LE(4);
    const indexOffset = Number(footer.readBigInt64LE(8));
    const indexSize = Number(footer.readBigInt64LE(16));
    const index = readAt(fd, indexOffset, indexSize);
    if (!sha1(index).equals(footer.subarray(24, 44))) throw new Error('index hash does not match the footer');
    let pos = 0;
    const readFString = () => {
        const len = index.readInt32LE(pos);
        pos += 4;
        if (len < 0) {
            const s = index.toString('utf16le', pos, pos - len * 2 - 2);
            pos += -len * 2;
            return s;
        }
        const s = index.toString('latin1', pos, pos + Math.max(0, len - 1));
        pos += len;
        return s;
    };
    const mount = readFString();
    const count = index.readInt32LE(pos);
    pos += 4;
    const entries = [];
    for (let i = 0; i < count; i++) {
        const name = readFString();
        const e = {
            name,
            offset: Number(index.readBigInt64LE(pos)),
            size: Number(index.readBigInt64LE(pos + 8)),
            uncompressedSize: Number(index.readBigInt64LE(pos + 16)),
            compression: index.readInt32LE(pos + 24),
            blocks: [],
        };
        pos += 48;
        if (e.compression !== 0) {
            const n = index.readInt32LE(pos);
            pos += 4;
            for (let k = 0; k < n; k++, pos += 16)
                e.blocks.push([Number(index.readBigInt64LE(pos)), Number(index.readBigInt64LE(pos + 8))]);
        }
        pos += 5;  // bEncrypted, CompressionBlockSize
        entries.push(e);
    }
    return { version, mount, entries };
}

function readEntry(fd, e) {
    if (e.compression === 0) {
        const headerSize = 53;
        return readAt(fd, e.offset + headerSize, e.size);
    }
    // version 3 block offsets are absolute; blocks are zlib streams
    return Buffer.concat(e.blocks.map(([start, end]) => zlib.inflateSync(readAt(fd, start, end - start))));
}

function create(outPath, files) {
    const parts = [];
    let offset = 0;
    const indexParts = [];
    writeFString(indexParts, kMountPoint);
    const count = Buffer.alloc(4);
    count.writeInt32LE(files.length);
    indexParts.push(count);
    for (const { pakPath, data } of files) {
        const hash = sha1(data);
        const header = entryHeader(0, data.length, hash);
        parts.push(header, data);
        writeFString(indexParts, pakPath);
        indexParts.push(entryHeader(offset, data.length, hash));
        offset += header.length + data.length;
    }
    const index = Buffer.concat(indexParts);
    const footer = Buffer.alloc(kFooterSize);
    footer.writeUInt32LE(kMagic, 0);
    footer.writeInt32LE(kVersion, 4);
    footer.writeBigInt64LE(BigInt(offset), 8);
    footer.writeBigInt64LE(BigInt(index.length), 16);
    sha1(index).copy(footer, 24);
    fs.writeFileSync(outPath, Buffer.concat([...parts, index, footer]));
}

module.exports = { readIndex, readEntry, create };
if (require.main !== module) return;

const [cmd, ...args] = process.argv.slice(2);
if (cmd === 'list') {
    const fd = fs.openSync(args[0], 'r');
    const { version, mount, entries } = readIndex(fd);
    console.log(`version ${version}, mount '${mount}', ${entries.length} entries`);
    for (const e of entries) console.log(`${e.name}\t${e.uncompressedSize}\t${e.compression ? 'zlib' : 'stored'}`);
} else if (cmd === 'extract') {
    const fd = fs.openSync(args[0], 'r');
    const e = readIndex(fd).entries.find((x) => x.name === args[1]);
    if (!e) throw new Error(`${args[1]} is not in ${args[0]}`);
    fs.writeFileSync(args[2], readEntry(fd, e));
} else if (cmd === 'create') {
    const files = args.slice(1).map((spec) => {
        const eq = spec.indexOf('=');
        return { pakPath: spec.slice(0, eq), data: fs.readFileSync(spec.slice(eq + 1)) };
    });
    create(args[0], files);
    // read it back the way the game will: index hash, then each entry's header and data hash
    const fd = fs.openSync(args[0], 'r');
    const check = readIndex(fd);
    for (const e of check.entries) {
        const want = files.find((f) => f.pakPath === e.name).data;
        if (!readEntry(fd, e).equals(want)) throw new Error(`read-back mismatch for ${e.name}`);
    }
    console.log(`wrote ${args[0]}: ${check.entries.map((e) => `${e.name} (${e.size} bytes)`).join(', ')}`);
} else {
    console.log('usage: node pak.js list <pak> | extract <pak> <path> <out> | create <out.pak> <path>=<file> ...');
    process.exitCode = 1;
}
