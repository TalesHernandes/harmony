// The Windows program's icon (D8). Writes windows/res/harmony.ico from
// tools/harmony-icon.svg, the same drawing as the PWA and the Android app.
//
// Rasterised by the Chromium Playwright already brings, like harmony-icons.mjs —
// no new dependency. An .ico is a directory of images, and since Windows Vista
// each image may simply be a PNG, so packing is a few lines of bytes. The sizes
// are the ones the shell asks for: taskbar, title bar, tray, Start, Alt+Tab, at
// 100–200% scale. The .ico is committed; run this only when the drawing changes:
//
//   PLAYWRIGHT_BROWSERS_PATH=... node tools/windows-icon.mjs
import { chromium } from '@playwright/test';
import { readFileSync, writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const out = join(here, '..', 'windows', 'res', 'harmony.ico');
const source = readFileSync(join(here, 'harmony-icon.svg'), 'utf8');
const sizes = [16, 20, 24, 32, 40, 48, 64, 256];

const browser = await chromium.launch();
const pngs = [];
try {
	for (const size of sizes) {
		const page = await browser.newPage({ viewport: { width: size, height: size } });
		const svg = source.replace('<svg ', `<svg width="${size}" height="${size}" `);
		await page.setContent(`<body style="margin:0">${svg}</body>`);
		pngs.push(await page.screenshot({ omitBackground: true }));
		await page.close();
	}
} finally {
	await browser.close();
}

// ICONDIR (6 bytes), one ICONDIRENTRY (16 bytes) per image, then the images.
const header = Buffer.alloc(6 + 16 * pngs.length);
header.writeUInt16LE(0, 0);
header.writeUInt16LE(1, 2); // 1 = icon
header.writeUInt16LE(pngs.length, 4);
let offset = header.length;
pngs.forEach((png, i) => {
	const at = 6 + 16 * i;
	const size = sizes[i];
	header.writeUInt8(size >= 256 ? 0 : size, at); // 0 means 256
	header.writeUInt8(size >= 256 ? 0 : size, at + 1);
	header.writeUInt8(0, at + 2); // no palette
	header.writeUInt8(0, at + 3);
	header.writeUInt16LE(1, at + 4); // planes
	header.writeUInt16LE(32, at + 6); // bits per pixel
	header.writeUInt32LE(png.length, at + 8);
	header.writeUInt32LE(offset, at + 12);
	offset += png.length;
});
mkdirSync(dirname(out), { recursive: true });
writeFileSync(out, Buffer.concat([header, ...pngs]));
console.log(`harmony.ico  ${sizes.join(', ')}  ${offset} bytes`);
