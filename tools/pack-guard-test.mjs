// Negative tests for mkfactory's kernel-extent guards.
//
// Why this exists: the CRC gates in cspboot only prove "the stored bytes are the
// bytes the header describes". They said yes to v9 and v10, and the box then
// answered `Wrong Image Format for bootm command` and sat there with link up and
// no ARP (docs/known-issues.md, KERNEL-EXTENT-NOT-A-UIMAGE). The packer is now
// the place that refuses such an extent, so the refusal itself is tested here
// instead of being trusted.
//
//   node tools/make_fixture.mjs <dir>          # writes template/kernel/rootfs
//   node tools/pack-guard-test.mjs <dir>       # packs mutated copies, expects refusal
//
// Three shapes are covered, one per way the extent can fail to boot:
//   1. no uImage header at all            - v9/v10 (a bare zImage)
//   2. uImage header, payload has no dtb  - v11 (header restored, DTB still missing)
//   3. uImage header + dtb, data CRC bad  - truncated/edited image; bootm says Bad Data CRC
import fs from 'node:fs';
import path from 'node:path';
import zlib from 'node:zlib';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const dir = process.argv[2];
if (!dir) {
  console.error('usage: node tools/pack-guard-test.mjs <fixture-dir>');
  process.exit(2);
}
const MK = fileURLToPath(new URL('./mkfactory.mjs', import.meta.url));
const crc = (b) => zlib.crc32(b) >>> 0;
const read = (n) => fs.readFileSync(path.join(dir, n));
const tmp = path.join(dir, 'guard');
fs.mkdirSync(tmp, { recursive: true });
const write = (n, b) => {
  const p = path.join(tmp, n);
  fs.writeFileSync(p, b);
  return p;
};

const template = path.join(dir, 'template.bin');
const rootfs = path.join(dir, 'rootfs.bin');
const kernel = read('kernel.bin');
const ihSize = kernel.readUInt32BE(12);
const payload = kernel.subarray(64, 64 + ihSize);

function pack(k) {
  try {
    execFileSync(process.execPath, [MK, 'pack', '--template', template, '--kernel', k,
      '--rootfs', rootfs, '--rootfs-at', '0x20000', '--header-at', '0x60000',
      '--out', path.join(tmp, 'out.bin')], { stdio: 'pipe' });
    return { ok: true, msg: '' };
  } catch (e) {
    return { ok: false, msg: (e.stdout || '') + (e.stderr || '') };
  }
}

// Shape 2 needs a uImage whose payload stops right before the dtb, with its own
// ih_size and ih_dcrc made consistent again - i.e. a *valid* uImage that simply
// carries a compressed kernel and nothing else.
const dtOfs = payload.lastIndexOf(Buffer.from('d00dfeed', 'hex'));
const noDtb = payload.subarray(0, dtOfs > 0 ? dtOfs : payload.length - 64);
const uiNoDtb = Buffer.concat([Buffer.from(kernel.subarray(0, 64)), noDtb]);
uiNoDtb.writeUInt32BE(noDtb.length, 12);
uiNoDtb.writeUInt32BE(crc(noDtb), 24);

const badCrc = Buffer.from(kernel);
badCrc[badCrc.length - 8] ^= 0xff;      // still ends with the dtb, data CRC no longer matches

const cases = [
  ['control: a bootable extent packs', write('k-good.bin', kernel), true, ''],
  ['bare zImage is refused (v9/v10)', write('k-bare.bin', Buffer.from(kernel.subarray(64))), false, 'not a legacy uImage'],
  ['uImage without a dtb is refused (v11)', write('k-nodtb.bin', uiNoDtb), false, 'device tree'],
  ['uImage with a stale data CRC is refused', write('k-badcrc.bin', badCrc), false, 'data CRC'],
];

let failed = 0;
for (const [label, file, wantOk, wantMsg] of cases) {
  const r = pack(file);
  const good = r.ok === wantOk && (!wantMsg || r.msg.includes(wantMsg));
  if (!good) {
    failed++;
    console.log(`  FAIL ${label}`);
    console.log(`       expected ${wantOk ? 'acceptance' : 'refusal containing ' + JSON.stringify(wantMsg)}`);
    console.log(`       got ${r.ok ? 'accepted' : 'refused: ' + r.msg.trim().split('\n').pop()}`);
  } else {
    console.log(`  ok   ${label}`);
  }
}
if (failed) {
  console.error(`pack guards: ${failed} of ${cases.length} cases wrong`);
  process.exit(1);
}
console.log(`pack guards: ${cases.length} cases behave as intended`);
