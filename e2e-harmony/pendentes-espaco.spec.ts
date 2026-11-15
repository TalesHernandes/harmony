import { test, expect, Person } from './fixtures';

test('day labels stay close to their messages', async ({ browser }) => {
	const ana = await Person.create(browser, 'dategapana');
	await ana.inChannel('geral').fill('Mensagem junto da data');
	await ana.page.getByRole('button', { name: /^enviar$/i }).click();
	await expect(ana.page.locator('[data-message]').filter({ hasText: 'Mensagem junto da data' })).toBeVisible();
	const row = ana.page.locator('[data-day-line] + [data-message]').first();
	await expect(row).toBeVisible();
	const spacing = await row.evaluate((el) => {
		const separator = el.previousElementSibling!;
		const label = separator.children[1];
		return {
			padding: getComputedStyle(separator).paddingBottom,
			margin: getComputedStyle(el).marginTop,
			gap: el.querySelector('[data-author]')!.getBoundingClientRect().top - label.getBoundingClientRect().bottom,
		};
	});
	const dimensions = await row.evaluate((el) => {
		const author = el.querySelector('[data-author]')!;
		const header = author.parentElement!;
		const stamp = author.nextElementSibling!;
		const body = el.querySelector('.text-message')!;
		return {
			nameToTime: stamp.getBoundingClientRect().left - author.getBoundingClientRect().right,
			headerHeight: header.getBoundingClientRect().height,
			timeHeight: stamp.getBoundingClientRect().height,
			headerToBody: body.getBoundingClientRect().top - header.getBoundingClientRect().bottom,
			timeToBody: body.getBoundingClientRect().top - stamp.getBoundingClientRect().bottom,
		};
	});
	expect(spacing).toEqual({ padding: '6px', margin: '0px', gap: 10 });
	await ana.close();
});
