import { test, expect, Person } from './fixtures';
import { mkdirSync, writeFileSync } from 'node:fs';

test('the voice island shares the call screen selector and aligns its rows', async ({ browser }) => {
  const person = await Person.create(browser, 'paritysevenc');
  const page = person.page;
  const output = 'windows/app/test-output/round7c-web';
  mkdirSync(output, { recursive: true });
  try {
    await person.joinVoice();
    // The island is also available while reading a text channel.
    const compact = page.locator('button[data-open="screen"]').first();
    const call = page.locator('[data-call] button[data-open="screen"]');
    await expect(compact).toHaveAttribute('aria-pressed', 'false');
    expect((await compact.boundingBox())!.height).toBe(32);
    expect((await call.boundingBox())!.height).toBe(44);
    await compact.click();
    await person.dialog().getByRole('button', { name: /compartilhar a tela/i }).click();
    await expect(compact).toHaveAttribute('aria-pressed', 'true');
    await expect(call).toHaveAttribute('aria-pressed', 'true');
    await page.locator('[data-account] [data-toggle="mic"]').click();
    const row = page.locator(`nav button[data-person="${person.username}"]`);
    await expect(row.locator('svg[data-icon="microphone-slash"]')).toBeVisible();
    const measures = await row.evaluate(el => {
      const rect = el.getBoundingClientRect();
      const name = el.querySelector(':scope > span')!.getBoundingClientRect();
      const icons = [...el.querySelectorAll('svg')].map(icon => { const r=icon.getBoundingClientRect(); return {center:r.y+r.height/2, height:r.height}; });
      return { width:rect.width, x:rect.x, radius:getComputedStyle(el).borderRadius, center:name.y+name.height/2, icons };
    });
    const text = page.locator('nav button[aria-label^="#"]').first();
    const textBox = (await text.boundingBox())!;
    expect(measures.width).toBe(textBox.width);
    expect(measures.x).toBe(textBox.x);
    for (const icon of measures.icons) expect(Math.abs(icon.center-measures.center)).toBeLessThanOrEqual(1);
    await row.hover();
    await page.screenshot({ path: `${output}/voice-sharing.png` });
    await text.click();
    await compact.click();
    await expect(person.dialog().getByRole('button', { name: /parar a tela/i })).toBeVisible();
    await person.dialog().getByRole('button', { name: /parar a tela/i }).click();
    await expect(compact).toHaveAttribute('aria-pressed', 'false');
    await page.screenshot({ path: `${output}/voice-stopped-in-chat.png` });
    writeFileSync(`${output}/measures.json`, JSON.stringify(measures,null,2));
  } finally { await person.close(); }
});
