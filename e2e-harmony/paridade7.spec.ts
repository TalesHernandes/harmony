import { test, expect, Person } from './fixtures';
import { mkdirSync, writeFileSync } from 'node:fs';

test('readable hierarchy and solid destructive actions across the main screens', async ({ browser }) => {
  const before = process.env.HARMONY_PARITY7_BEFORE === '1';
  const output = `windows/app/test-output/round7-web-${before ? 'before' : 'after'}`;
  mkdirSync(output, { recursive: true });
  const person = await Person.create(browser, 'parityseven');
  const page = person.page;
  await page.setViewportSize({ width: 1280, height: 800 });
  await page.evaluate(() => document.fonts.ready);
  const measures: Record<string, unknown> = {};
  async function capture(name: string) {
    await page.screenshot({ path: `${output}/${name}.png` });
    const texts = await page.locator('body *').evaluateAll(elements => elements.flatMap(el => {
      if (!Array.from(el.childNodes).some(node => node.nodeType === Node.TEXT_NODE && node.textContent?.trim())) return [];
      const rect = el.getBoundingClientRect(), css = getComputedStyle(el);
      if (!rect.width || !rect.height || css.visibility !== 'visible') return [];
      return [{ tag: el.tagName, weight: Number(css.fontWeight), size: css.fontSize }];
    }));
    measures[name] = texts;
    if (!before) for (const text of texts) expect(text.weight).toBeGreaterThanOrEqual(500);
  }
  await capture('chat');
  await page.getByRole('button', { name: 'emoji', exact: true }).click();
  const query = page.getByRole('textbox', { name: 'procurar emoji', exact: true });
  await query.fill('sdfsdfsd');
  measures.emojiGeometry = await query.evaluate(el => {
    const box = el.getBoundingClientRect(), field = el.parentElement!.getBoundingClientRect();
    return { input: box.toJSON(), field: field.toJSON(), fontSize: getComputedStyle(el).fontSize, lineHeight: getComputedStyle(el).lineHeight, weight: getComputedStyle(el).fontWeight };
  });
  await capture('emoji');
  await page.keyboard.press('Escape');
  await page.getByRole('button', { name: 'Meu perfil', exact: true }).click();
  const profile = page.getByRole('dialog', { name: /perfil de/i });
  await capture('profile');
  const close = profile.getByRole('button', { name: 'fechar', exact: true });
  if (!before) {
    await expect(close).toHaveCSS('background-color', 'rgb(219, 59, 62)');
    await expect(close).toHaveCSS('color', 'rgb(252, 252, 252)');
    await close.hover();
    await expect(close).toHaveCSS('background-color', 'rgb(225, 84, 81)');
    await expect(profile.locator('p').first()).toHaveCSS('font-weight', '600');
  }
  await capture('close-hover');
  await close.click();
  await page.getByRole('button', { name: /ajustes/i }).first().click();
  await page.getByRole('button', { name: 'Conta', exact: true }).click();
  await capture('account');
  writeFileSync(`${output}/measures.json`, JSON.stringify(measures, null, 2));
  await person.close();
});
