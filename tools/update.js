// Keeps Limbo, and the backend on the host's PC, up to date from GitHub's latest releases (Play Limbo.bat runs it
// before its menu):
//   node update.js <Limbo folder> <the folder next to it (OT6.5)>
// Each install names its release in its VERSION file (the release zips carry it, and this writes it after an
// update). A folder without one, or a git checkout (a developer's), is left alone, and so is everything when GitHub
// can't be reached or out\no-update.txt exists. An update downloads the release's zip, checks it against the release's
// .sha256, unpacks it (Windows' tar) and copies its files over the old ones: what isn't in the zip stays (Limbo's out\
// settings and logs; the backend's data, .env and node_modules). The backend is updated only while it isn't running,
// and its packages are installed again when its package.json changed.
// Exit code 2: Play Limbo.bat itself was replaced, so it starts again; 3: something else was updated.
const { execFileSync, execSync } = require('child_process');
const crypto = require('crypto');
const fs = require('fs');
const net = require('net');
const os = require('os');
const path = require('path');

const kOwner = 'ArielS-gif';
const kTimeoutMs = 8000;

const [limboDir, rootDir] = process.argv.slice(2).map((p) => path.resolve(p));

const capital = (text) => text[0].toUpperCase() + text.slice(1);

function parseVersion(text) {
    const m = /^v(\d+)\.(\d+)\.(\d+)$/.exec(String(text || '').trim());
    return m ? m.slice(1).map(Number) : null;
}

function newer(a, b) {  // a > b
    for (let i = 0; i < 3; i++) if (a[i] !== b[i]) return a[i] > b[i];
    return false;
}

async function get(url) {
    const res = await fetch(url, { headers: { 'User-Agent': 'Limbo-updater', Accept: 'application/vnd.github+json' },
                                   signal: AbortSignal.timeout(kTimeoutMs * 4) });
    if (!res.ok) throw new Error(`${url}: HTTP ${res.status}`);
    return res;
}

// 127.0.0.1:3551 answering means this PC's backend runs (its files are in use).
function backendRunning() {
    return new Promise((resolve) => {
        const socket = net.connect({ host: '127.0.0.1', port: 3551, timeout: 1000 }, () => { socket.destroy(); resolve(true); });
        socket.on('error', () => resolve(false));
        socket.on('timeout', () => { socket.destroy(); resolve(false); });
    });
}

function unzip(zip, into) {
    const tar = path.join(process.env.SystemRoot || 'C:\\Windows', 'System32', 'tar.exe');
    if (fs.existsSync(tar)) execFileSync(tar, ['-xf', zip, '-C', into], { stdio: 'ignore' });
    else execFileSync('powershell.exe', ['-NoProfile', '-Command', `Expand-Archive -LiteralPath '${zip}' -DestinationPath '${into}' -Force`],
                      { stdio: 'ignore' });
}

// Copies every file under `from` over `to`; returns the relative paths whose contents changed.
function copyOver(from, to, base = '') {
    const changed = [];
    for (const entry of fs.readdirSync(path.join(from, base), { withFileTypes: true })) {
        const relative = path.join(base, entry.name);
        if (entry.isDirectory()) {
            fs.mkdirSync(path.join(to, relative), { recursive: true });
            changed.push(...copyOver(from, to, relative));
            continue;
        }
        const data = fs.readFileSync(path.join(from, relative));
        const target = path.join(to, relative);
        if (fs.existsSync(target) && fs.readFileSync(target).equals(data)) continue;
        fs.writeFileSync(target, data);
        changed.push(relative);
    }
    return changed;
}

// One install: { name, repo, dir, asset (the zip's name for a tag), root (the folder inside the zip) }.
async function update(install) {
    const { name, repo, dir } = install;
    if (!fs.existsSync(dir) || fs.existsSync(path.join(dir, '.git'))) return null;  // none, or a developer's checkout
    const versionFile = path.join(dir, 'VERSION');
    const local = fs.existsSync(versionFile) ? fs.readFileSync(versionFile, 'utf8').trim() : '';
    if (!parseVersion(local)) return null;  // not from a release: left alone
    const release = await (await get(`https://api.github.com/repos/${kOwner}/${repo}/releases/latest`)).json();
    const latest = release.tag_name;
    if (!parseVersion(latest) || !newer(parseVersion(latest), parseVersion(local))) {
        console.log(` ${capital(name)} ${local} is up to date.`);
        return null;
    }
    const zipName = install.asset(latest);
    const zipAsset = release.assets.find((a) => a.name === zipName);
    const hashAsset = release.assets.find((a) => a.name === `${zipName}.sha256`);
    if (!zipAsset || !hashAsset) throw new Error(`${repo} ${latest} has no ${zipName} with its .sha256`);
    console.log(` Updating ${name} ${local} -> ${latest}...`);
    const zip = Buffer.from(await (await get(zipAsset.browser_download_url)).arrayBuffer());
    const expected = (await (await get(hashAsset.browser_download_url)).text()).trim().split(/\s+/)[0].toLowerCase();
    const actual = crypto.createHash('sha256').update(zip).digest('hex');
    if (actual !== expected) throw new Error(`${zipName} doesn't match its .sha256; not installed`);

    const work = fs.mkdtempSync(path.join(os.tmpdir(), 'limbo-update-'));
    try {
        const zipPath = path.join(work, zipName);
        fs.writeFileSync(zipPath, zip);
        fs.mkdirSync(path.join(work, 'x'));
        unzip(zipPath, path.join(work, 'x'));
        const unpacked = path.join(work, 'x', install.root);
        if (!fs.existsSync(unpacked)) throw new Error(`${zipName} has no ${install.root} folder`);
        const changed = copyOver(unpacked, dir);
        fs.writeFileSync(versionFile, `${latest}\r\n`);
        console.log(` ${capital(name)} is now ${latest} (${changed.length} file(s) changed).`);
        return { latest, changed };
    } finally {
        fs.rmSync(work, { recursive: true, force: true });
    }
}

(async () => {
    if (process.env.LIMBO_NO_UPDATE || fs.existsSync(path.join(limboDir, 'out', 'no-update.txt'))) return;
    let restart = false, updated = false;
    try {
        const limbo = await update({ name: 'Limbo', repo: 'Limbo', dir: limboDir, asset: (tag) => `Limbo-${tag}.zip`, root: 'Limbo' });
        restart = Boolean(limbo && limbo.changed.some((file) => file.toLowerCase() === 'play limbo.bat'));
        updated = Boolean(limbo);
    } catch (e) {
        console.log(` Couldn't update Limbo: ${e.message}`);
    }
    try {
        const backendDir = ['Limbo-Backend', 'Limbo-Backend-main'].map((name) => path.join(rootDir, name))
            .find((dir) => fs.existsSync(path.join(dir, 'index.js')));
        if (backendDir && !fs.existsSync(path.join(backendDir, '.git'))) {
            if (await backendRunning()) {
                console.log(' The backend is running, so it is not updated now (close its window to let it update).');
            } else {
                const packageFile = path.join(backendDir, 'package.json');
                const before = fs.existsSync(packageFile) ? fs.readFileSync(packageFile, 'utf8') : '';
                const backend = await update({ name: 'the backend', repo: 'Limbo-Backend', dir: backendDir, asset: () => 'Limbo-Backend.zip',
                                               root: 'Limbo-Backend' });
                updated = updated || Boolean(backend);
                if (backend && fs.readFileSync(packageFile, 'utf8') !== before) {
                    console.log(' Installing the backend\'s packages again...');
                    execSync('npm install --no-audit --no-fund --loglevel=error', { cwd: backendDir, stdio: 'inherit' });
                }
            }
        }
    } catch (e) {
        console.log(` Couldn't update the backend: ${e.message}`);
    }
    process.exitCode = restart ? 2 : updated ? 3 : 0;
})();
