// The two screens a signed-out person can reach: sign in, and sign up with an
// invite. Which of them exists at all is the server's decision — /signup
// without a usable code answers 404 and this file is never loaded — so nothing
// here is a security check, only a form.

import { useState } from 'preact/hooks';
import { api, message } from './api';
import { ForgotPassword } from './Security';
import { log } from './diag';

// In the new drawing since 24/09/2026: a field is a step of surface with no
// outline, and says it has the cursor with the same inset line the typing box
// uses; the button is the kit's 44px blue one.
export const field =
	'h-11 w-full rounded-control bg-surface px-3 text-body text-foreground outline-none focus:ring-1 focus:ring-inset focus:ring-outline-muted';
export const button =
	'h-11 w-full rounded-control bg-accent px-4 text-meta font-semibold text-foreground hover:bg-accent-hover disabled:cursor-default disabled:bg-default disabled:text-foreground-muted';
export const label = 'mb-1 block text-meta text-foreground-muted';

export function Frame({ title, subtitle, children }: { title: string; subtitle: string; children: any }) {
	return (
		<div class="min-h-full grid place-items-center p-6">
			<div class="w-full max-w-sm">
				<h1 class="text-title text-foreground">{title}</h1>
				<h2 class="mb-6 mt-1 text-meta text-foreground-muted">{subtitle}</h2>
				{children}
			</div>
		</div>
	);
}

export function Login() {
	const [forgot, setForgot] = useState(false);
	const [challenge, setChallenge] = useState('');
	const [method, setMethod] = useState('');
	const [code, setCode] = useState('');
	const [recovery, setRecovery] = useState(false);
	const [username, setUsername] = useState('');
	const [password, setPassword] = useState('');
	const [busy, setBusy] = useState(false);
	const [error, setError] = useState('');

	async function submit(e: Event) {
		e.preventDefault();
		setBusy(true);
		setError('');
		try {
			if (challenge) await api.login2FA(challenge, code);
			else {
				const result = await api.login(username, password);
				setPassword('');
				if (result.challenge) { setChallenge(result.challenge); setMethod(result.twofa ?? ''); setBusy(false); return; }
			}
			log('login ok');
			location.href = '/';
		} catch (err) {
			setError(message(err));
			setBusy(false);
		}
	}

	if (forgot) return <ForgotPassword onBack={() => setForgot(false)} />;
	return (
		<Frame title="harmony" subtitle="Entre com a sua conta.">
			<form class="space-y-4" onSubmit={submit}>
				{challenge ? <div class="space-y-4">
					<div>
						<label class={label} for="factor-code">{recovery ? 'Código de recuperação' : method === 'email' ? 'Código recebido por e-mail' : 'Código do app'}</label>
						<input id="factor-code" class={field} autocomplete="one-time-code" value={code} onInput={(e) => setCode(e.currentTarget.value)} />
					</div>
					{method === 'totp' && <button type="button" class="text-meta text-foreground-muted" onClick={() => { setRecovery(!recovery); setCode(''); }}>{recovery ? 'Usar o código do app' : 'Usar um código de recuperação'}</button>}
				</div> : <>
				<div>
					<label class={label} for="u">Nome ou e-mail</label>
					<input
						id="u"
						class={field}
						autocomplete="username"
						autocapitalize="none"
						value={username}
						onInput={(e) => setUsername((e.target as HTMLInputElement).value)}
					/>
				</div>
				<div>
					<label class={label} for="p">Senha</label>
					<input
						id="p"
						class={field}
						type="password"
						autocomplete="current-password"
						value={password}
						onInput={(e) => setPassword((e.target as HTMLInputElement).value)}
					/>
				</div>
				</>}
				{error && <p class="text-meta text-danger-soft-fg">{error}</p>}
				<button class={button} disabled={busy || (challenge ? !code : !username || !password)}>
					{busy ? 'Entrando…' : 'Entrar'}
				</button>
			</form>
			<button type="button" class="mt-4 text-meta text-foreground-muted" onClick={() => setForgot(true)}>Esqueci a senha</button>
			<p class="mt-6 text-meta text-foreground-muted">
				Não existe cadastro aberto. Para entrar é preciso um convite do dono.
			</p>
		</Frame>
	);
}

export function Signup() {
	const [email, setEmail] = useState('');
	const code = new URLSearchParams(location.search).get('code') ?? '';
	const [username, setUsername] = useState('');
	const [display, setDisplay] = useState('');
	const [password, setPassword] = useState('');
	const [busy, setBusy] = useState(false);
	const [error, setError] = useState('');

	async function submit(e: Event) {
		e.preventDefault();
		setBusy(true);
		setError('');
		try {
			await api.signup(code, username, display, password, email);
			log('signup ok');
			location.href = '/';
		} catch (err) {
			setError(message(err));
			setBusy(false);
		}
	}

	return (
		<Frame title="Criar a sua conta" subtitle="Você foi convidado. O convite vale uma vez só.">
			<form class="space-y-4" onSubmit={submit}>
				<div><label class={label} for="email">E-mail</label><input id="email" type="email" required class={field} autocomplete="email" value={email} onInput={(e) => setEmail(e.currentTarget.value)} /></div>
				<div>
					<label class={label} for="u">Usuário</label>
					<input
						id="u"
						class={field}
						autocomplete="username"
						autocapitalize="none"
						placeholder="Ex.: joao"
						value={username}
						onInput={(e) => setUsername((e.target as HTMLInputElement).value)}
					/>
					<p class="mt-1 text-meta text-foreground-muted">É por ele que as pessoas te chamam: @joao.</p>
				</div>
				<div>
					<label class={label} for="d">Nome que aparece</label>
					<input
						id="d"
						class={field}
						placeholder="Ex.: João"
						value={display}
						onInput={(e) => setDisplay((e.target as HTMLInputElement).value)}
					/>
				</div>
				<div>
					<label class={label} for="p">Senha</label>
					<input
						id="p"
						class={field}
						type="password"
						autocomplete="new-password"
						value={password}
						onInput={(e) => setPassword((e.target as HTMLInputElement).value)}
					/>
					<p class="mt-1 text-meta text-foreground-muted">
						Pelo menos 10 caracteres.
					</p>
				</div>
				{error && <p class="text-meta text-danger-soft-fg">{error}</p>}
				<button class={button} disabled={busy || !username || !password || !email}>
					{busy ? 'Criando…' : 'Criar conta'}
				</button>
			</form>
		</Frame>
	);
}
