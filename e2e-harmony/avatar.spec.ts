import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { test, expect, Person } from './fixtures';

test('foto muda ao vivo nas pessoas, na sala e no perfil, e pode ser removida', async ({ browser }) => {
	const ana = await Person.create(browser, 'fotoana');
	const bruno = await Person.create(browser, 'fotobru');
	await ana.joinVoice();
	await bruno.joinVoice();
	await ana.page.locator('button[data-person="fotoana"]').first().click();
	const card = ana.page.getByRole('dialog');
	await card.locator('[data-face]').click();
	await expect(card.getByRole('menu', { name: 'Foto de perfil' })).toBeVisible();
	await expect(card.getByRole('button', { name: 'Alterar foto' })).toBeVisible();
	const picker = card.locator('[data-avatar-picker]');
	await picker.setInputFiles({ name: 'wrong.webp', mimeType: 'image/webp', buffer: Buffer.from('RIFF0000WEBPVP8 ') });
	await expect(card.getByRole('alert')).toContainText('WebP não é aceito');
	await picker.setInputFiles({ name: 'large.png', mimeType: 'image/png', buffer: Buffer.alloc(5 * 1024 * 1024 + 1) });
	await expect(card.getByRole('alert')).toContainText('5 MiB');
	const file = { name: 'photo.png', mimeType: 'image/png', buffer: readFileSync(join(__dirname, '../internal/harmony/api/assets/app/icon-192.png')) };
	await picker.setInputFiles(file);
	await ana.page.getByRole('dialog', { name: 'Editar foto', exact: true }).getByRole('button', { name: 'Salvar', exact: true }).click();
	const photo = card.locator('[data-face] img');
	await expect(photo).toBeVisible();
	await expect(photo).toHaveJSProperty('naturalWidth', 256);
	const versionURL = await photo.getAttribute('src');
	const remote = bruno.page.locator('button[data-person="fotoana"] img').first();
	await expect(remote).toHaveAttribute('src', versionURL!);
	await expect(remote).toHaveJSProperty('naturalWidth', 256);
	await expect(ana.page.locator('[data-account] img')).toHaveAttribute('src', versionURL!);
	await expect(remote).not.toHaveAttribute('style');
	await expect(card.locator('input[data-volume]')).toHaveCount(0);

	// Failed images return to initials; a new version can load again.
	await bruno.page.route('**/api/people/fotoana/avatar?*', (route) => route.abort());
	await picker.setInputFiles(file);
	await ana.page.getByRole('dialog', { name: 'Editar foto', exact: true }).getByRole('button', { name: 'Salvar', exact: true }).click();
	await expect(photo).not.toHaveAttribute('src', versionURL!);
	await expect(bruno.page.locator('button[data-person="fotoana"] img')).toHaveCount(0);
	await bruno.page.unroute('**/api/people/fotoana/avatar?*');
	await picker.setInputFiles(file);
	await ana.page.getByRole('dialog', { name: 'Editar foto', exact: true }).getByRole('button', { name: 'Salvar', exact: true }).click();
	await expect(remote).toHaveJSProperty('naturalWidth', 256);
	await card.getByRole('button', { name: 'Remover foto' }).click();
	await expect(photo).toHaveCount(0);
	await expect(ana.page.locator('[data-account] img')).toHaveCount(0);
	await expect(bruno.page.locator('button[data-person="fotoana"] img')).toHaveCount(0);
	await bruno.close();
	await ana.close();
});

test('photo editor crops before upload and live messages show photos without reload', async ({ browser }, info) => {
 const ana = await Person.create(browser, 'cropana', { hasTouch: true });
 const bruno = await Person.create(browser, 'cropbru');
 await ana.page.locator('aside [data-person="cropana"]').first().click();
 const profile = ana.page.getByRole('dialog', { name: 'perfil de cropana', exact: true });
 // A nonsquare image with distinct halves makes dragging observable.
 const data = await ana.page.evaluate(() => {
  const canvas = document.createElement('canvas'); canvas.width = 800; canvas.height = 400;
  const ctx = canvas.getContext('2d')!; ctx.fillStyle = '#ff0000'; ctx.fillRect(0,0,400,400); ctx.fillStyle = '#0000ff'; ctx.fillRect(400,0,400,400);
  return canvas.toDataURL('image/png').split(',')[1];
 });
 const file = { name: 'crop.png', mimeType: 'image/png', buffer: Buffer.from(data, 'base64') };
 let uploads = 0;
 ana.page.on('request', req => { if (req.method() === 'PUT' && req.url().endsWith('/api/me/avatar')) { uploads++; } });
 await ana.page.evaluate(() => {
  const original = window.fetch;
  window.fetch = async (input, init) => {
   if (String(input).endsWith('/api/me/avatar') && init?.method === 'PUT' && init.body instanceof File) {
    const file = init.body; const image = await createImageBitmap(file); const bytes = new Uint8Array(await file.arrayBuffer());
    (window as unknown as { __photoUpload: unknown }).__photoUpload = { type: file.type, dimensions: [image.width, image.height], magic: [...bytes.slice(0,2)] }; image.close();
   }
   return original(input, init);
  };
 });
 const picker = profile.locator('[data-avatar-picker]');
 await picker.setInputFiles(file);
 const editor = ana.page.getByRole('dialog', { name: 'Editar foto', exact: true });
 await expect(editor.locator('canvas')).toBeVisible();
 await expect(editor.getByRole('button', { name: 'Salvar', exact: true })).toBeEnabled();
 expect(uploads).toBe(0);
 await ana.page.keyboard.press('Escape');
 await expect(editor).toHaveCount(0); await expect(profile).toBeVisible(); expect(uploads).toBe(0);
 await picker.setInputFiles(file);
 const canvas = editor.locator('canvas');
 await expect(editor.getByRole('button', { name: 'Salvar', exact: true })).toBeEnabled();
 const original = await canvas.evaluate(el => (el as HTMLCanvasElement).toDataURL());
 await editor.getByLabel('Zoom da foto').fill('2');
 await expect.poll(() => canvas.evaluate(el => (el as HTMLCanvasElement).toDataURL())).not.toBe(original);
 const box = (await canvas.boundingBox())!;
 await ana.page.mouse.move(box.x + 100, box.y + 100); await ana.page.mouse.down(); await ana.page.mouse.move(box.x + 150, box.y + 120); await ana.page.mouse.up();
 await editor.screenshot({ path: info.outputPath('photo-editor.png') });
 const dragged = await canvas.evaluate(el => (el as HTMLCanvasElement).toDataURL());
 await ana.page.mouse.wheel(0, -100);
 await expect.poll(() => editor.getByLabel('Zoom da foto').inputValue()).not.toBe('2');
 // Send real touch input so the browser exercises pointer capture too.
 const touch = await ana.context.newCDPSession(ana.page);
 await touch.send('Input.dispatchTouchEvent', { type: 'touchStart', touchPoints: [{ x: box.x + 150, y: box.y + 120 }] });
 await touch.send('Input.dispatchTouchEvent', { type: 'touchMove', touchPoints: [{ x: box.x + 80, y: box.y + 100 }] });
 await touch.send('Input.dispatchTouchEvent', { type: 'touchEnd', touchPoints: [] });
 await touch.detach();
 await expect.poll(() => canvas.evaluate(el => (el as HTMLCanvasElement).toDataURL())).not.toBe(dragged);
 await editor.getByRole('button', { name: 'Salvar', exact: true }).click();
 await expect(editor).toHaveCount(0);
 expect(uploads).toBe(1);
 const uploaded = await ana.page.evaluate(() => (window as unknown as { __photoUpload: unknown }).__photoUpload);
 expect(uploaded).toEqual({ type: 'image/jpeg', dimensions: [512,512], magic: [255,216] });
 await expect(profile.locator('[data-face] img')).toHaveJSProperty('naturalWidth',256);
 await profile.locator('[data-face]').click();
 await profile.getByRole('button', {name: 'Ver foto', exact:true}).click();
 const lightbox = ana.page.locator('[data-photo-lightbox]');
 await expect(lightbox.locator('img')).toHaveJSProperty('naturalWidth',256);
 await ana.page.keyboard.press('Escape'); await expect(lightbox).toHaveCount(0); await expect(profile).toBeVisible();
 await profile.locator('[data-face]').click(); await profile.getByRole('button', {name:'Ver foto',exact:true}).click(); await lightbox.click({ position: { x: 10, y: 10 } }); await expect(lightbox).toHaveCount(0);
 await profile.getByRole('button', { name: 'fechar', exact: true }).click();
 await ana.page.getByPlaceholder(/Escrever em/).fill('photo live regression'); await ana.page.keyboard.press('Enter');
 const row = () => bruno.page.locator('[data-message]').filter({ hasText: 'photo live regression' }).last();
 await expect(row().locator('[data-face] img')).toHaveJSProperty('naturalWidth',256);
 await row().locator('[data-message-person]').click();
 await expect(bruno.page.getByRole('dialog', { name: 'perfil de cropana' })).toBeVisible();
 await bruno.page.keyboard.press('Escape');
 await bruno.page.reload();
 await expect(row().locator('[data-face] img')).toHaveJSProperty('naturalWidth',256);
 await row().hover(); await row().getByRole('button', { name: 'responder', exact: true }).click();
 await bruno.page.getByPlaceholder(/Escrever em/).fill('reply photo regression'); await bruno.page.keyboard.press('Enter');
 const reply = bruno.page.locator('[data-message]').filter({ hasText: 'reply photo regression' }).last();
 await expect(reply.locator('[data-quote] [data-face] img')).toHaveJSProperty('naturalWidth',256);
 await bruno.page.getByRole('button', { name: 'procurar', exact: true }).click();
 const search = bruno.page.locator('[data-search-panel]'); await search.locator('[data-search-box]').fill('photo live regression');
 await expect(search.locator('[data-hit] [data-face] img')).toHaveJSProperty('naturalWidth',256);
 await search.screenshot({ path: info.outputPath('photo-search.png') });
 await bruno.close(); await ana.close();
});
