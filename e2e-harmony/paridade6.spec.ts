import { test, expect, Person } from './fixtures';
import { mkdirSync } from 'node:fs';

test('message hover stays separated from the composer at every scroll position', async ({ browser }) => {
  const person = await Person.create(browser, 'parity6gap');
  const page = person.page;
  await page.setViewportSize({ width: 1280, height: 800 });
  for (let i = 0; i < 16; i++) {
    await person.inChannel('geral').fill(`parity6 message ${i}\nsecond line of the scrolling fixture`);
    await page.getByRole('button', { name: /^enviar$/i }).click();
    await expect(page.locator('[data-message]').filter({ hasText: `parity6 message ${i}` })).toBeVisible();
    await page.waitForTimeout(350); // Respect the same posting limit as the native audit fixture.
  }
  const output = process.env.HARMONY_PARITY6_WEB_OUTPUT || 'windows/app/test-output/round6-web-after';
  mkdirSync(output, { recursive: true });
  const gaps: number[] = [];
  expect(await page.locator('[data-history]').evaluate((el) => el.scrollHeight > el.clientHeight)).toBe(true);
  for (const position of ['bottom', 'middle'] as const) {
    await page.locator('[data-history]').evaluate((el, at) => { el.scrollTop = at === 'bottom' ? el.scrollHeight : (el.scrollHeight - el.clientHeight) / 2; }, position);
    const viewport = await page.locator('[data-message-viewport]').boundingBox();
    const composer = await page.locator('[data-composer-island]').boundingBox();
    gaps.push(composer!.y - (viewport!.y + viewport!.height));
    const y = await page.locator('[data-message]').evaluateAll((rows, bottom) => {
      const visible = rows.map((row) => row.getBoundingClientRect()).filter((r) => r.top < bottom && r.bottom > 100);
      return Math.min(visible[visible.length - 1].bottom, bottom) - 3;
    }, viewport!.y + viewport!.height);
    await page.mouse.move(viewport!.x + 80, y);
    await page.screenshot({ path: `${output}/${position}-hover.png` });
  }
  expect(await page.locator('[data-history]').evaluate(el => getComputedStyle(el).paddingBottom)).toBe('0px');
  expect(gaps).toEqual([expect.any(Number), expect.any(Number)]);
  for (const gap of gaps) expect(gap).toBeGreaterThanOrEqual(8);
  await person.close();
});


test('self profile and close buttons share the requested palette and body weight', async ({ browser }) => {
  const person = await Person.create(browser, 'parity6profile');
  const page = person.page;
  await page.getByRole('button', { name: 'Meu perfil', exact: true }).click();
  const profile = page.getByRole('dialog', { name: /perfil de/i });
  await expect(profile).toBeVisible();
  await expect(profile.getByRole('button', { name: 'Ligar', exact: true })).toHaveCount(0);
  await profile.locator('[data-face]').hover();
  await expect(profile.locator('.profile-photo-edit')).toHaveCSS('opacity', '1');
  await profile.locator('[data-face]').click();
  await expect(profile.getByRole('menu')).toBeVisible();
  await expect(profile.getByRole('button', { name: 'Ver foto', exact: true })).toHaveCount(0);
  const close = profile.getByRole('button', { name: 'fechar', exact: true });
  await expect(close).toHaveCSS('background-color', 'rgb(219, 59, 62)');
  await expect(close).toHaveCSS('color', 'rgb(252, 252, 252)');
  await close.hover();
  await expect(close).toHaveCSS('background-color', 'rgb(225, 84, 81)');
  await expect(close).toHaveCSS('color', 'rgb(252, 252, 252)');
  await expect(profile.locator('p').first()).toHaveCSS('font-weight', '600');
  await page.screenshot({path:'windows/app/test-output/round6-web-after/self-profile.png'});
  await close.click();
  await expect(profile).toHaveCount(0);
  await person.close();
});


test('pending attachment opens the same viewer before sending', async ({ browser }) => {
 const person=await Person.create(browser,'parity6preview'); const page=person.page;
 let messagesSent=0;page.on('request',request=>{if(request.method()==='POST' && /\/api\/channels\/[^/]+\/messages$/.test(request.url())) messagesSent++;});
 await page.locator('[data-composer] input[type="file"]').setInputFiles({name:'preview.png',mimeType:'image/png',buffer:Buffer.from('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+aD1sAAAAASUVORK5CYII=','base64')});
 await page.getByRole('button',{name:'Ampliar preview.png',exact:true}).click();
 const viewer=page.locator('[data-lightbox]'); await expect(viewer).toBeVisible();
 await expect(viewer.locator('img')).toHaveJSProperty('naturalWidth',1);
 expect(messagesSent).toBe(0);
 await page.keyboard.press('Escape'); await expect(viewer).toHaveCount(0);
 await expect(page.locator('[data-upload="preview.png"]')).toBeVisible();
 await person.close();
});


test('the other profile starts a direct call', async ({browser}) => {
 const caller=await Person.create(browser,'parity6dialer');
 const callee=await Person.create(browser,'parity6callee');
 await caller.page.locator('aside button[data-person="parity6callee"]').first().click();
 const profile=caller.page.getByRole('dialog',{name:'perfil de parity6callee',exact:true});
 await profile.getByRole('button',{name:'Ligar',exact:true}).click();
 await expect(profile).toHaveCount(0);
 await expect(callee.page.locator('[data-incoming-call]')).toBeVisible();
 await callee.page.locator('[data-call-reject]').click();
 await caller.close();await callee.close();
});
