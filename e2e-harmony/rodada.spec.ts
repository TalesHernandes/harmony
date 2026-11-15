import { test, expect, Person } from './fixtures';

test('measure channel gaps and avatar alignment', async ({ browser }) => {
 const ana = await Person.create(browser, 'measureana');
 await ana.page.getByPlaceholder(/Escrever em/).fill('measurement message');
 await ana.page.keyboard.press('Enter');
 await ana.page.getByText('measurement message', { exact: true }).waitFor();
 const measure = () => ana.page.evaluate(() => {
  const rect = (e: Element) => e.getBoundingClientRect();
  const gap = (selector: string) => {
   const glyph = document.querySelector(selector)!;
   const wrapper = glyph.closest('span[aria-hidden]')!;
   return rect(wrapper.nextElementSibling!).left - rect(glyph).right;
  };
  const delta = (face: Element, text: Element) => (rect(face).top + rect(face).bottom - rect(text).top - rect(text).bottom) / 2;
  const account = document.querySelector('[data-account]')!;
  const person = document.querySelector('aside [data-person="measureana"]')!;
  const msg = [...document.querySelectorAll('[data-message]')].find(e => e.textContent?.includes('measurement message'))!;
  return { headerGap: gap('header [data-icon="hashtag"]'), listGap: gap('nav [data-icon="hashtag"]'), accountCenterDelta: delta(account.querySelector('[data-face]')!, account.querySelector('.account-name')!), peopleCenterDelta: delta(person.querySelector('[data-face]')!, person.querySelector('[data-face]')!.nextElementSibling!), messageTopDelta: rect(msg.querySelector('[data-face]')!).top - rect(msg.querySelector('[data-author]')!).top };
 });
 const after = await measure();
 console.log('LAYOUT before', JSON.stringify({ headerGap: 15, listGap: 6, accountCenterDelta: 0, peopleCenterDelta: 0, messageTopDelta: 6 }), 'after', JSON.stringify(after));
 expect(after.headerGap).toBe(7.5); expect(after.listGap).toBe(3);
 expect(after.accountCenterDelta).toBe(0); expect(after.peopleCenterDelta).toBe(0); expect(after.messageTopDelta).toBe(6); // centred on name + first line since 03/10/2026 (keen-albattani)
 await ana.page.locator('aside [data-person="measureana"]').first().click();
 const profileDelta = await ana.page.getByRole('dialog').evaluate(card => { const face = card.querySelector('[data-face]')!.getBoundingClientRect(); const text = card.querySelector('[data-face]')!.nextElementSibling!.getBoundingClientRect(); return (face.top + face.bottom - text.top - text.bottom) / 2; });
 console.log('PROFILE center delta before -4 after', profileDelta);
 expect(profileDelta).toBe(0);
 await ana.page.keyboard.press('Escape');
 await ana.page.locator('button[aria-label="ajustes"]').first().click();
 const settings = ana.page.locator('[data-settings]');
 await expect(settings.locator('[data-reset-volumes], [data-reset-stream-volumes]')).toHaveCount(0);
 const sizes = await settings.locator('label:has(input[type="checkbox"])').evaluateAll(rows => rows.map(row => ({ size: getComputedStyle(row).fontSize, center: (() => { const a=row.getBoundingClientRect(); const b=row.querySelector('input')!.getBoundingClientRect(); return (a.top+a.bottom-b.top-b.bottom)/2; })() })));
 console.log('SETTINGS switch labels', JSON.stringify(sizes));
 expect(sizes.length).toBe(4); for (const row of sizes) { expect(row.size).toBe('15px'); expect(row.center).toBe(0); }
 await settings.getByRole('button', { name: 'fechar', exact: true }).click();
 await ana.page.getByRole('button', { name: 'procurar', exact: true }).click();
 const search = ana.page.locator('[data-search-panel]');
 const edges = await search.evaluate(panel => ({ field: panel.querySelector('input')!.getBoundingClientRect().left, hint: panel.querySelector('[role="status"] p')!.getBoundingClientRect().left }));
 console.log('SEARCH edges', JSON.stringify(edges)); expect(edges.hint).toBe(edges.field);
 await ana.close();
});

