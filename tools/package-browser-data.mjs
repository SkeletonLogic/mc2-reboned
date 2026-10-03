// PORT: browser delivery manifest -- MANIFEST v2, NAMED PACKS.
//
// OWNER RULING 2026-08-21 (C2/C3 first pass): "we do want to package the audio,
// it should be present". That put data/sound, data/movies and mc2res.txt into
// the delivered tree, and it still holds -- the file list below is unchanged.
//
// OWNER RULING 2026-09-02 (C2, the distribution question that one deferred):
// "Package it all, but include the capability for us to pivot to 3 easily if
// need be. In that same vein, we should have download options for players to
// download both pieces so that some players will already have it if we ever
// need to stop giving it to them." Option 3 was: the player supplies their own
// audio from their own retail install, so nothing retail is served.
//
// THAT IS WHY THIS EMITS PACKS RATHER THAN ONE FLAT LIST.
//
// The pivot does NOT belong in the audio backend. That seam is about the
// DEVICE -- a WebAudio peer is simply a third implementation beside null/ and
// win32/. Where the BYTES come from is delivery, which is here. So each pack
// carries a `source`, and throwing the switch is editing one word:
//
//     "source": "bundled"   fetch it from us          (today)
//     "source": "local"     the player supplies it    (the pivot)
//
// No rebuild, no code change, and nothing to migrate for players who already
// installed -- installs are keyed on a per-pack version marker, not on where
// the bytes came from.
//
// PER-PACK VERSIONS ARE LOAD-BEARING. v1 had ONE `.mc2-version` marker for the
// whole install, so adding or changing any pack would have re-fetched all
// 429.6 MiB. Each pack now carries its own digest and the shell keeps one
// marker per pack.
//
// WHAT KEEPS THIS LOCAL TODAY: classic-mc2-port/.gitignore is a single "*", so
// the delivered tree is never committed. Serving it beyond localhost is the
// decision the 2026-09-02 ruling answers, and the packs are what make that
// answer reversible.
//
// Still excluded on purpose: executables, DLLs, logs, crash dumps and
// developer captures. Those are not game data.
//
//   node package-browser-data.mjs SOURCE OUTPUT [--archives]
//
// --archives additionally writes packs/<id>.zip, the downloadable copy a player
// keeps. It is OPT-IN because it rewrites ~200 MB and the normal build must not
// pay for that on every link.
import { cp, mkdir, readdir, readFile, stat, writeFile, open } from 'node:fs/promises';
import path from 'node:path';
import crypto from 'node:crypto';

const args = process.argv.slice(2);
const wantArchives = args.includes('--archives');
const [sourceRoot, outputRoot] = args.filter(a => !a.startsWith('--'));
if (!sourceRoot || !outputRoot) {
  throw new Error('usage: package-browser-data.mjs SOURCE OUTPUT [--archives]');
}

const roots = [
  'art.fst', 'camera.fst', 'effect.fst', 'insignia.fst', 'misc.fst',
  'mission.fst', 'textures.fst', 'tgl.fst', 'badcard.csv', 'vidcard.csv',
  'options.cfg', 'prefs.cfg', 'system.cfg', 'HiPrefs.cfg', 'MidPrefs.cfg',
  'MinPrefs.cfg', 'mc2_settings.bin', 'testTxm.tga',
  'data/art', 'data/effects', 'data/missions', 'data/multiplayer',
  // data/savegame is deliberately NOT here. Retail ships no saves (the game
  // creates the folder), and the source tree's folder holds the owner's own
  // playthrough saves -- 28 files, 4 MB, measured 2026-09-24 -- which must not
  // reach a public build, and which made the NEW CAMPAIGN save dialog open
  // every one of them on a fresh install.
  'data/objects', 'data/terrain', 'data/textures',
  'assets/Graphics',
  // C2: the audio the browser build has been silent without. 200 MiB of pak
  // banks and streamed wavs -- the whole of what SoundSystem asks for.
  'data/sound',
  // Same ruling, same reasoning: the converted VP9/Opus movie set. The .tga
  // stills beside them are what MC2 falls back to and cost almost nothing.
  'data/movies',
  // C3 DISSOLVES RATHER THAN NEEDING A WORKAROUND. Font FILENAMES come from
  // the retail string table, which is why browser text was blocked -- but the
  // table is 244 KB and shipping it gives the browser every string, not just
  // the 11 font names. No hard-coded name list, no partial extract.
  'mc2res.txt'
];

// The two audio pieces the ruling names, split because they are the two halves
// a player might want separately -- and because music is the half most likely
// to be dropped first if the answer ever changes.
//
// `required: false` on both is what makes "local" a survivable state rather
// than a broken one: the game must start and play with an audio pack absent.
// data/movies is deliberately left in core for now -- the ruling was about
// audio, and making it a fourth pack would be scope the owner did not ask for.
// It is the obvious next candidate if the same question is ever asked of it.
const PACKS = [
  { id: 'core',        label: 'Game data',         required: true  },
  { id: 'audio-voice', label: 'Voice and effects', required: false },
  { id: 'audio-music', label: 'Music',             required: false }
];

function packFor(relative) {
  if (!relative.startsWith('data/sound/')) return 'core';
  const name = path.basename(relative).toLowerCase();
  return name.startsWith('music') ? 'audio-music' : 'audio-voice';
}

const files = [];
async function collect(relative) {
  const source = path.join(sourceRoot, relative);
  const info = await stat(source);
  if (info.isDirectory()) {
    for (const entry of await readdir(source)) await collect(path.join(relative, entry));
  } else if (info.isFile()) {
    files.push({ path: relative.replaceAll('\\', '/'), size: info.size });
  }
}
for (const root of roots) await collect(root);
files.sort((a, b) => a.path.localeCompare(b.path));

for (const file of files) {
  const source = path.join(sourceRoot, file.path);
  const destination = path.join(outputRoot, file.path);
  await mkdir(path.dirname(destination), { recursive: true });
  let current;
  try { current = await stat(destination); } catch {}
  if (!current || current.size !== file.size) await cp(source, destination);
}

const packs = PACKS.map(meta => {
  const own = files.filter(f => packFor(f.path) === meta.id);
  return {
    id: meta.id,
    label: meta.label,
    required: meta.required,
    // Every pack ships bundled today. This one word is the pivot.
    source: 'bundled',
    version: crypto.createHash('sha256').update(JSON.stringify(own)).digest('hex').slice(0, 16),
    bytes: own.reduce((n, f) => n + f.size, 0),
    files: own
  };
});

// OWNER PLAYTESTS ONLY: SOURCE/playtest-saves/*.fit, when that folder exists,
// becomes an optional fourth pack installed as data/savegame/<name>. It is how
// a playtest hands the owner named saves without their touching any file. The
// owner's own data/savegame stays excluded (see the note on `roots`), and this
// pack is never written into --archives, so it cannot reach a shared copy.
// The version hashes the file BYTES, so re-armed saves reinstall.
{
  const savesDir = path.join(sourceRoot, 'playtest-saves');
  let names = [];
  try { names = (await readdir(savesDir)).filter(n => n.toLowerCase().endsWith('.fit')).sort(); } catch {}
  if (names.length) {
    const hash = crypto.createHash('sha256');
    const own = [];
    for (const name of names) {
      const body = await readFile(path.join(savesDir, name));
      hash.update(name).update(body);
      const relative = `data/savegame/${name}`;
      await mkdir(path.join(outputRoot, 'data/savegame'), { recursive: true });
      await writeFile(path.join(outputRoot, relative), body);
      own.push({ path: relative, size: body.length });
    }
    packs.push({ id: 'playtest-saves', label: 'Playtest saves', required: false, source: 'bundled',
                 version: hash.digest('hex').slice(0, 16), bytes: own.reduce((n, f) => n + f.size, 0),
                 files: own, playtestOnly: true });
    console.log(`[browser-data] playtest-saves: ${names.length} save(s): ${names.join(', ')}`);
  }
}

const manifest = {
  manifestVersion: 2,
  // Kept so a shell that predates packs still reports something sane rather
  // than undefined; it is not what installs are keyed on any more.
  version: crypto.createHash('sha256').update(JSON.stringify(files)).digest('hex').slice(0, 16),
  bytes: files.reduce((n, f) => n + f.size, 0),
  packs
};
await mkdir(outputRoot, { recursive: true });
await writeFile(path.join(outputRoot, 'manifest.json'), JSON.stringify(manifest));

for (const pack of packs) {
  console.log(`[browser-data]   ${pack.id.padEnd(12)} ${String(pack.files.length).padStart(5)} files  ` +
              `${(pack.bytes / 1048576).toFixed(1).padStart(7)} MiB  source=${pack.source}  ${pack.version}`);
}
console.log(`[browser-data] ${files.length} files, ${manifest.bytes} bytes, version ${manifest.version}`);

// ---------------------------------------------------------------------------
// The downloadable copy.
//
// A real .zip, STORED (no compression), written with no dependency. Stored
// entries are deliberate: the payload is wav and pak data that deflate barely
// touches, so compressing would burn minutes to save almost nothing, and a
// stored archive is trivial for the shell to read back on import.
// ---------------------------------------------------------------------------
const CRC_TABLE = (() => {
  const table = new Int32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
    table[n] = c;
  }
  return table;
})();
function crc32(buffer) {
  let c = -1;
  for (let i = 0; i < buffer.length; i++) c = CRC_TABLE[(c ^ buffer[i]) & 0xFF] ^ (c >>> 8);
  return (c ^ -1) >>> 0;
}

async function writeArchive(pack) {
  const target = path.join(outputRoot, 'packs', `${pack.id}.zip`);
  await mkdir(path.dirname(target), { recursive: true });
  const handle = await open(target, 'w');
  let offset = 0;
  const central = [];

  for (const file of pack.files) {
    const data = await readFile(path.join(sourceRoot, file.path));
    const name = Buffer.from(file.path, 'utf8');
    const sum = crc32(data);

    const local = Buffer.alloc(30);
    local.writeUInt32LE(0x04034b50, 0);
    local.writeUInt16LE(20, 4);            // version needed
    local.writeUInt16LE(0, 6);             // flags
    local.writeUInt16LE(0, 8);             // method 0 = stored
    local.writeUInt32LE(0, 10);            // dos time/date, left at zero
    local.writeUInt32LE(sum, 14);
    local.writeUInt32LE(data.length, 18);
    local.writeUInt32LE(data.length, 22);
    local.writeUInt16LE(name.length, 26);
    local.writeUInt16LE(0, 28);
    await handle.write(local);
    await handle.write(name);
    await handle.write(data);

    const entry = Buffer.alloc(46);
    entry.writeUInt32LE(0x02014b50, 0);
    entry.writeUInt16LE(20, 4);
    entry.writeUInt16LE(20, 6);
    entry.writeUInt16LE(0, 8);
    entry.writeUInt16LE(0, 10);
    entry.writeUInt32LE(0, 12);
    entry.writeUInt32LE(sum, 16);
    entry.writeUInt32LE(data.length, 20);
    entry.writeUInt32LE(data.length, 24);
    entry.writeUInt16LE(name.length, 28);
    entry.writeUInt32LE(offset, 42);
    central.push(Buffer.concat([entry, name]));

    offset += local.length + name.length + data.length;
  }

  const directory = Buffer.concat(central);
  await handle.write(directory);
  const end = Buffer.alloc(22);
  end.writeUInt32LE(0x06054b50, 0);
  end.writeUInt16LE(central.length, 8);
  end.writeUInt16LE(central.length, 10);
  end.writeUInt32LE(directory.length, 12);
  end.writeUInt32LE(offset, 16);
  await handle.write(end);
  await handle.close();

  const written = offset + directory.length + end.length;
  console.log(`[browser-data] packs/${pack.id}.zip  ${(written / 1048576).toFixed(1)} MiB, ${pack.files.length} entries`);
}

if (wantArchives) {
  for (const pack of packs) if (!pack.playtestOnly) await writeArchive(pack);
}
