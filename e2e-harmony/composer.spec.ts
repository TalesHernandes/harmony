// The box you type in, and the one thing it must never do: lose what was typed.

import { test, expect, Person } from './fixtures';

// What is on this device, as the app itself would read it.
const onDisk = (who: Person) =>
	who.page.evaluate(() => localStorage.getItem('harmony.drafts'));

test('escrever no instante em que a conversa abre não perde o que foi escrito', async ({
	browser,
}) => {
	const ana = await Person.create(browser, 'frestana');
	const bruno = await Person.create(browser, 'frestabru');
	await bruno.page.getByPlaceholder(/Escrever em/).waitFor({ timeout: 15_000 });

	// The trap is set before the conversation is opened, and it types into the
	// box **in the same breath the box is born**.
	//
	// That instant is the whole test. A component's effects run after the
	// browser has painted, so there is a gap between the box being on screen —
	// typeable, focusable, real — and anything that runs afterwards to prepare
	// it. A person hits that gap by being quick; this hits it every time,
	// because a MutationObserver runs before the paint an effect waits for.
	//
	// It sets the value through the prototype's own setter rather than the
	// property, because the framework holds the node and a plain assignment is
	// the one thing it cannot see.
	await bruno.page.evaluate((nome) => {
		const alvo = 'Conversar com ' + nome;
		const escreva = () => {
			const ta = document.querySelector('[data-composer] textarea') as HTMLTextAreaElement | null;
			if (!ta || !ta.placeholder.includes(alvo)) return false;
			const setter = Object.getOwnPropertyDescriptor(
				HTMLTextAreaElement.prototype,
				'value',
			)?.set;
			setter?.call(ta, 'escrito na fresta');
			ta.dispatchEvent(new Event('input', { bubbles: true }));
			return true;
		};
		const obs = new MutationObserver(() => {
			if (escreva()) obs.disconnect();
		});
		obs.observe(document.body, {
			childList: true,
			subtree: true,
			attributes: true,
			attributeFilter: ['placeholder'],
		});
	}, 'frestana');

	await bruno.openDMWith('frestana');

	// A second is far longer than the gap: whatever would wipe it has long
	// since run.
	await bruno.page.waitForTimeout(1_000);
	await expect(bruno.composerFor('frestana')).toHaveValue('escrito na fresta');

	// And it is a real draft and not just text sitting in a box: it sends, and
	// it sends into the conversation that was opened.
	await bruno.page.getByRole('button', { name: /^enviar$/i }).click();
	await expect(bruno.page.locator('[data-message]').last()).toContainText('escrito na fresta', {
		timeout: 15_000,
	});

	await bruno.close();
	await ana.close();
});

test('o rascunho espera você na conversa, e não aparece na outra', async ({ browser }) => {
	const ana = await Person.create(browser, 'rascana');
	const bruno = await Person.create(browser, 'rascbru');
	await bruno.inChannel('geral').waitFor({ timeout: 15_000 });

	// One sentence in the channel…
	await bruno.inChannel('geral').fill('metade de uma frase no geral');
	await bruno.openDMWith('rascana');
	const conversa = bruno.composerFor('rascana');
	await conversa.waitFor({ timeout: 15_000 });

	// …and the conversation does not inherit it. This is the half that matters
	// most: a draft that followed you out would mean somebody's private sentence
	// appearing in a channel.
	await expect(conversa).toHaveValue('');
	await conversa.fill('outra metade, esta no privado');

	// Back to the channel: its own half-sentence is where it was left.
	await bruno.openChannels();
	await bruno.page.locator('nav button[data-space="server"]').first().click();
	await bruno.page.locator('nav button[aria-label="#geral"]').first().click();
	await expect(bruno.inChannel('geral')).toHaveValue('metade de uma frase no geral', {
		timeout: 15_000,
	});

	// And so is the conversation's.
	await bruno.openMessages();
	await bruno.page.locator('nav button', { hasText: 'rascana' }).first().click();
	await expect(bruno.composerFor('rascana')).toHaveValue('outra metade, esta no privado', {
		timeout: 15_000,
	});

	// Nothing of this reached the device: the app is in front of you the whole
	// time, and that is the window in which the disk is meant to hold nothing.
	expect(await onDisk(bruno)).toBeNull();

	await bruno.close();
	await ana.close();
});

test('o rascunho sobrevive a fechar o app, e sai junto com a conta', async ({ browser }) => {
	const ana = await Person.create(browser, 'discoana');
	await ana.inChannel('geral').waitFor({ timeout: 15_000 });
	await ana.inChannel('geral').fill('isto tem que estar aqui quando eu voltar');

	// Leaving. pagehide is the signal a telephone actually delivers when it
	// throws the page out of memory, and dispatching it here is the only way to
	// stand inside that moment without really closing the page.
	await ana.page.evaluate(() => window.dispatchEvent(new Event('pagehide')));
	expect(await onDisk(ana)).toContain('isto tem que estar aqui quando eu voltar');

	// Coming back the hard way: the page is thrown away and built again.
	await ana.page.reload();
	await expect(ana.inChannel('geral')).toHaveValue('isto tem que estar aqui quando eu voltar', {
		timeout: 20_000,
	});

	// And the copy in the file is gone the moment it is back in the box. On the
	// disk only while the app is not in front of you — that is the rule, and
	// this is the line that holds it.
	expect(await onDisk(ana)).toBeNull();

	// Leaving again, so there is something for the sign-out to take away.
	await ana.page.evaluate(() => window.dispatchEvent(new Event('pagehide')));
	expect(await onDisk(ana)).not.toBeNull();

	await ana.page.locator('button[aria-label="ajustes"]').first().click();
	// The tab you left open is remembered per device (D4h), so the tab this is
	// about is asked for by name rather than trusted to be the default.
	await ana.page.locator('[data-tab="conta"]').click();
	await ana.page.getByRole('button', { name: /sair desta conta/i }).click();
	// Leaving asks first (D4v); the answer is the red button in the box.
	await ana.page.locator('[data-confirm] [data-confirm-yes]').click();
	// Logout and an in-flight 401 may both navigate here. Assert the final
	// document instead of subscribing to the load event of the first navigation.
	await expect(ana.page).toHaveURL(/\/login/, { timeout: 15_000 });
	await expect(ana.page.getByRole('button', { name: 'Entrar', exact: true })).toBeVisible({ timeout: 15_000 });

	// Nothing typed on a shared computer outlives the session it was typed in.
	expect(await onDisk(ana)).toBeNull();

	await ana.close();
});
