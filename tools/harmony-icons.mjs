// harmony's PWA icons. Writes web/harmony/public/icon-*.png from
// tools/harmony-icon.svg.
//
// The mark has curves the old per-pixel drawing could not express, so the SVG
// is rasterised by the Chromium that Playwright already brings for the e2e
// suite — no new dependency. Run it with `npm run icons:harmony` from the repo
// root, with PLAYWRIGHT_BROWSERS_PATH set as for the e2e tests; the PNGs are
// committed, so this only needs to run when the drawing itself changes.
import { chromium } from '@playwright/test';
import { readFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const outDir = join(here, '..', 'web', 'harmony', 'public');
const source = readFileSync(join(here, 'harmony-icon.svg'), 'utf8');

// purpose "any" is the drawing as it is: a rounded tile with transparent
// corners. purpose "maskable" is a full-bleed square, because the launcher
// cuts its own shape; the tile becomes a square covering the whole viewBox,
// and the mark shrinks a tenth around the tile's centre (547.5, 552.5) to stay
// clear of a tight circle (the safe zone is the central circle of radius 40%).
function variant({ maskable }) {
	if (!maskable) return source;
	return source
		.replace(/<path id="tile"[^>]*\/>/, '<rect x="96" y="101" width="903" height="903" fill="#051939"/>')
		.replace('<g id="mark">', '<g id="mark" transform="translate(54.75 55.25) scale(0.9)">');
}

const icons = [
	['icon-192.png', 192, { maskable: false }],
	['icon-512.png', 512, { maskable: false }],
	['icon-maskable-512.png', 512, { maskable: true }],
];

mkdirSync(outDir, { recursive: true });
const browser = await chromium.launch();
try {
	for (const [name, size, opts] of icons) {
		const page = await browser.newPage({ viewport: { width: size, height: size } });
		const svg = variant(opts).replace('<svg ', `<svg width="${size}" height="${size}" `);
		await page.setContent(`<body style="margin:0">${svg}</body>`);
		const path = join(outDir, name);
		await page.screenshot({ path, omitBackground: true });
		await page.close();
		console.log(`${name}  ${size}x${size}`);
	}
} finally {
	await browser.close();
}
