import { useEffect, useMemo, useState } from 'preact/hooks';
import { encode } from 'uqr';
import { api, ApiError, message, type User } from './api';
import { Frame, field, button, label } from './Auth';

// Beside the one blue button of a screen: a grey one for the other way
// forward, and plain text for leaving, like "Esqueci a senha" on the login.
const secondary = 'h-11 w-full rounded-control bg-default px-4 text-meta font-medium text-foreground hover:bg-default-hover disabled:cursor-default disabled:text-foreground-muted';
const textLink = 'text-meta text-foreground-muted hover:text-foreground';

// The authenticator's QR, drawn as one SVG path from the module matrix: no
// innerHTML and no inline style, so the page's CSP has nothing to object to.
// Black on white in both themes on purpose — an inverted code is the one
// many phone cameras refuse to read.
function TotpQR({ uri }: { uri: string }) {
 const qr = useMemo(() => encode(uri, { ecc: 'M', border: 2 }), [uri]);
 const d = qr.data.flatMap((row, y) => row.flatMap((on, x) => on ? [`M${x} ${y}h1v1h-1z`] : [])).join('');
 return <svg class="size-48 self-center rounded-control" viewBox={`0 0 ${qr.size} ${qr.size}`} shape-rendering="crispEdges" role="img" aria-label="QR code para o app autenticador">
  <rect width={qr.size} height={qr.size} fill="#fff" /><path d={d} fill="#000" />
 </svg>;
}

function ErrorLine({ text }: { text: string }) { return text ? <p role="alert" class="text-meta text-danger-soft-fg">{text}</p> : null; }

export function ForgotPassword({ onBack }: { onBack: () => void }) {
 const [identifier, setIdentifier] = useState('');
 const [sent, setSent] = useState(false);
 const [busy, setBusy] = useState(false);
 const [error, setError] = useState('');
 return <Frame title="harmony" subtitle="Esqueci a senha">
  {sent ? <p class="text-meta text-foreground-muted">Se a conta tem e-mail confirmado, enviaremos um link. Confira também o spam.</p> : <form class="space-y-4" onSubmit={async (e) => {
   e.preventDefault(); setBusy(true); setError('');
   try { await api.forgotPassword(identifier); setSent(true); } catch (err) { setError(message(err)); } finally { setBusy(false); }
  }}>
   <div><label class={label} for="forgot-identifier">Nome ou e-mail</label><input id="forgot-identifier" class={field} autocomplete="username" required value={identifier} onInput={(e) => setIdentifier(e.currentTarget.value)} /></div>
   <ErrorLine text={error} /><button class={button} disabled={busy}>Enviar link</button>
  </form>}
  <button class={'mt-4 ' + textLink} onClick={onBack}>Voltar para entrar</button>
 </Frame>;
}

export function ResetPassword({ token }: { token: string }) {
 const [password, setPassword] = useState('');
 const [code, setCode] = useState('');
 const [needsCode, setNeedsCode] = useState(false);
 const [busy, setBusy] = useState(false);
 const [done, setDone] = useState(false);
 const [error, setError] = useState('');
 return <Frame title="harmony" subtitle="Nova senha">
  {done ? <div class="space-y-4"><p class="text-meta text-foreground-muted">Senha redefinida. Entre de novo.</p><a class={button + ' flex items-center justify-center'} href="/login">Entrar</a></div> : <form class="space-y-4" onSubmit={async (e) => {
   e.preventDefault(); setBusy(true); setError('');
   try { await api.resetPassword(token, password, code); setPassword(''); setCode(''); setDone(true); }
   catch (err) { if (err instanceof ApiError && err.code === 'totp_required') setNeedsCode(true); setError(message(err)); }
   finally { setBusy(false); }
  }}>
   <div><label class={label} for="reset-password">Nova senha</label><input id="reset-password" class={field} type="password" autocomplete="new-password" minLength={10} required value={password} onInput={(e) => setPassword(e.currentTarget.value)} /></div>
   {needsCode && <div><label class={label} for="reset-code">Código do app ou de recuperação</label><input id="reset-code" class={field} autocomplete="one-time-code" required value={code} onInput={(e) => setCode(e.currentTarget.value)} /></div>}
   <ErrorLine text={error} /><button class={button} disabled={busy}>Salvar nova senha</button>
  </form>}
 </Frame>;
}

export function EmailVerification({ user, required, onDone, onSkip }: { user: User; required: boolean; onDone: () => void; onSkip?: () => void }) {
 const [email, setEmail] = useState('');
 const [password, setPassword] = useState('');
 const [code, setCode] = useState('');
 const [sent, setSent] = useState(!!user.email);
 const [busy, setBusy] = useState(false);
 const [error, setError] = useState('');
 const [info, setInfo] = useState('');
 const [changing, setChanging] = useState(false);
 async function run(action: () => Promise<void>) { setBusy(true); setError(''); try { await action(); } catch (err) { setError(message(err)); } finally { setBusy(false); } }
 return <Frame title="harmony" subtitle="Confirme seu e-mail">
  <div class="space-y-4">
  <p class="text-meta text-foreground-muted">{sent && !changing ? `Digite o código que enviamos para ${email || user.email}. Ele vale por 10 minutos; confira também o spam.` : 'Cadastre seu e-mail para recuperar a conta e receber avisos de segurança.'}</p>
  {/* Two keys, two forms: without them Preact reuses one <form> for both,
      and the click on "Usar outro e-mail" lands as a submit of the empty new one. */}
  {!sent || changing ? <form key="address" class="space-y-4" onSubmit={(e) => { e.preventDefault(); void run(async () => { await api.changeEmail(password, email); setPassword(''); setSent(true); setChanging(false); setInfo('Código enviado.'); }); }}>
   <div><label class={label} for="verification-email">E-mail</label><input id="verification-email" class={field} type="email" autocomplete="email" required value={email} onInput={(e) => setEmail(e.currentTarget.value)} /></div>
   <div><label class={label} for="verification-password">Senha atual</label><input id="verification-password" class={field} type="password" autocomplete="current-password" required value={password} onInput={(e) => setPassword(e.currentTarget.value)} /></div>
   <button class={button} disabled={busy}>Enviar código</button>
  </form> : <form key="code" class="space-y-4" onSubmit={(e) => { e.preventDefault(); void run(async () => { await api.verifyEmail(code); onDone(); }); }}>
   <div><label class={label} for="verification-code">Código</label><input id="verification-code" class={field} autocomplete="one-time-code" inputMode="numeric" required value={code} onInput={(e) => setCode(e.currentTarget.value)} /></div>
   <button class={button} disabled={busy}>Confirmar e-mail</button>
   <button type="button" class={secondary} disabled={busy} onClick={() => void run(async () => { await api.resendEmail(); setInfo('Código reenviado.'); })}>Reenviar</button>
   <button type="button" class={secondary} disabled={busy} onClick={() => setChanging(true)}>Usar outro e-mail</button>
  </form>}
  <ErrorLine text={error} />{info && <p role="status" class="text-meta text-foreground-muted">{info}</p>}
  <div class="flex justify-between gap-4 pt-2">
   {!required && onSkip ? <button class={textLink} onClick={onSkip}>Agora não</button> : <span />}
   <button class={"danger-action " + (textLink)} disabled={busy} onClick={() => void run(async () => { await api.logout(); location.href = '/login'; })}>Sair da conta</button>
  </div>
  </div>
 </Frame>;
}

// The settings' own vocabulary (Panels.tsx): 8px pieces on the raised ground,
// a row that says what is set and one button that opens only that change.
// A field inside an open card is one step up, so it reads against it.
const card = 'rounded-control bg-surface-2 p-3';
const small = 'h-8 shrink-0 rounded-control bg-default px-3 text-meta font-medium text-foreground hover:bg-default-hover disabled:text-foreground-muted';
const inner = 'field h-11 w-full rounded-control bg-surface-3 px-3 text-item text-foreground placeholder:text-foreground-muted';
const go = 'h-11 rounded-control bg-accent px-4 text-[14px] font-semibold text-foreground hover:bg-accent-hover disabled:bg-default disabled:text-foreground-muted';
const quiet = 'h-11 rounded-control bg-default px-4 text-meta font-medium text-foreground hover:bg-default-hover disabled:text-foreground-muted';
const hint = 'text-meta text-foreground-muted';

type Open = '' | 'email' | 'password' | 'twofa';

function Row({ title, value, note, action, onAction, disabled }: { title: string; value: string; note?: string; action: string; onAction: () => void; disabled?: boolean }) {
 return <div class={card + ' flex items-center gap-3'}>
  <div class="min-w-0 flex-1">
   <p class={hint}>{title}</p>
   <p class="truncate text-item text-foreground">{value}</p>
   {note && <p class={hint}>{note}</p>}
  </div>
  <button class={small + (action === "Cancelar" ? " danger-action" : "")} disabled={disabled} onClick={onAction}>{action}</button>
 </div>;
}

function Field({ label, ...input }: { label: string } & Record<string, any>) {
 return <label class="flex flex-col gap-1.5"><span class={hint}>{label}</span><input class={inner} {...input} /></label>;
}

export function AccountSecurity() {
 const [user, setUser] = useState<User | null>(null);
 const [open, setOpen] = useState<Open>('');
 const [password, setPassword] = useState('');
 const [email, setEmail] = useState('');
 const [next, setNext] = useState('');
 const [code, setCode] = useState('');
 const [codeSent, setCodeSent] = useState(false);
 const [setup, setSetup] = useState<{ secret: string; uri: string } | null>(null);
 const [codes, setCodes] = useState<string[]>([]);
 const [busy, setBusy] = useState(false);
 const [error, setError] = useState('');
 const [info, setInfo] = useState('');
 useEffect(() => { void api.me().then((r) => setUser(r.user)).catch((err) => setError(message(err))); }, []);

 // Opening one change closes the other and forgets what was typed in it: a
 // password left in a field nobody is looking at is a password on screen.
 function toggle(which: Open) {
  setOpen(open === which ? '' : which);
  setPassword(''); setEmail(''); setNext(''); setCode(''); setCodeSent(false); setSetup(null); setError(''); setInfo('');
 }
 async function run(action: () => Promise<void>) { setBusy(true); setError(''); setInfo(''); try { await action(); } catch (err) { setError(message(err)); } finally { setBusy(false); } }
 async function factor(action: string) {
  const r = await api.factor(action, password, code);
  if (r.secret && r.uri) { setSetup({ secret: r.secret, uri: r.uri }); setCode(''); return; }
  if (r.code_required) { setCodeSent(true); setInfo('Enviamos um código para o seu e-mail.'); return; }
  if (r.user) setUser(r.user);
  setCodes(r.recovery_codes ?? []);
  setOpen(''); setSetup(null); setPassword(''); setCode(''); setCodeSent(false);
  setInfo(action === 'disable' ? 'Verificação em duas etapas desligada.' : 'Verificação em duas etapas atualizada.');
 }
 if (!user) return error ? <p class="text-meta text-danger-soft-fg">{error}</p> : null;

 const twofa = user.twofa === 'totp' ? 'Por app autenticador' : user.twofa === 'email' ? 'Por e-mail' : 'Desligada';
 return <section class="flex flex-col gap-2" aria-label="Conta e segurança">
  <h3 class="text-label uppercase text-foreground-muted">Conta e segurança</h3>
  <div class="flex flex-col gap-0.5">
   <Row title="E-mail" value={user.email || 'não cadastrado'} note={user.email_verified ? undefined : 'não confirmado'} action={open === 'email' ? 'Cancelar' : 'Trocar'} onAction={() => toggle('email')} />
   {open === 'email' && <div class={card + ' flex flex-col gap-3'}>
    {!codeSent ? <form class="flex flex-col gap-3" onSubmit={(e) => { e.preventDefault(); void run(async () => { await api.changeEmail(password, email); setPassword(''); setCodeSent(true); setInfo('Enviamos um código para o novo e-mail.'); }); }}>
     <Field label="Novo e-mail" type="email" required autocomplete="email" value={email} onInput={(e: any) => setEmail(e.currentTarget.value)} />
     <Field label="Senha atual" type="password" required autocomplete="current-password" value={password} onInput={(e: any) => setPassword(e.currentTarget.value)} />
     <button class={go} disabled={busy || !email || !password}>Enviar código</button>
    </form> : <form class="flex flex-col gap-3" onSubmit={(e) => { e.preventDefault(); void run(async () => { const r = await api.verifyEmail(code); setUser(r.user); setOpen(''); setCode(''); setCodeSent(false); setInfo('E-mail trocado.'); }); }}>
     <Field label="Código recebido" autocomplete="one-time-code" inputMode="numeric" required value={code} onInput={(e: any) => setCode(e.currentTarget.value)} />
     <button class={go} disabled={busy || !code}>Confirmar</button>
     <button type="button" class={quiet} disabled={busy} onClick={() => void run(async () => { await api.resendEmail(); setInfo('Código reenviado.'); })}>Reenviar código</button>
    </form>}
   </div>}

   <Row title="Senha" value="••••••••••" action={open === 'password' ? 'Cancelar' : 'Trocar'} onAction={() => toggle('password')} />
   {open === 'password' && <form class={card + ' flex flex-col gap-3'} onSubmit={(e) => { e.preventDefault(); void run(async () => { await api.changePassword(password, next); setOpen(''); setPassword(''); setNext(''); setInfo('Senha trocada. Os outros aparelhos saíram da conta.'); }); }}>
    <Field label="Senha atual" type="password" required autocomplete="current-password" value={password} onInput={(e: any) => setPassword(e.currentTarget.value)} />
    <Field label="Nova senha (pelo menos 10 caracteres)" type="password" required minLength={10} autocomplete="new-password" value={next} onInput={(e: any) => setNext(e.currentTarget.value)} />
    <button class={go} disabled={busy || !password || next.length < 10}>Salvar nova senha</button>
   </form>}

   <Row title="Verificação em duas etapas" value={twofa} note={!user.email_verified ? 'confirme o e-mail antes' : undefined} action={open === 'twofa' ? 'Cancelar' : user.twofa ? 'Mudar' : 'Ligar'} disabled={!user.email_verified} onAction={() => toggle('twofa')} />
   {open === 'twofa' && <div class={card + ' flex flex-col gap-3'}>
    {!user.twofa && !setup && <>
     <p class={hint}>Além da senha, entrar vai pedir um código. Escolha de onde ele vem.</p>
     <Field label="Senha atual" type="password" autocomplete="current-password" value={password} onInput={(e: any) => setPassword(e.currentTarget.value)} />
     <button class={go} disabled={busy || !password} onClick={() => void run(() => factor('totp/setup'))}>Por app autenticador (recomendado)</button>
     <button class={quiet} disabled={busy || !password} onClick={() => void run(() => factor('email/enable'))}>Por e-mail</button>
    </>}
    {setup && <form class="flex flex-col gap-3" onSubmit={(e) => { e.preventDefault(); void run(() => factor('totp/enable')); }}>
     <p class={hint}>No app autenticador (Google Authenticator, Aegis…), aponte a câmera para o código. Vale por 10 minutos.</p>
     <TotpQR uri={setup.uri} />
     <p class={hint}>Sem câmera? Digite esta chave no app:</p>
     <p class="break-words font-mono text-item text-foreground">{setup.secret.match(/.{1,4}/g)?.join(' ')}</p>
     <a class={quiet + ' flex items-center justify-center'} href={setup.uri}>Abrir no app autenticador</a>
     <Field label="Código que o app mostra" autocomplete="one-time-code" inputMode="numeric" required value={code} onInput={(e: any) => setCode(e.currentTarget.value)} />
     <button class={go} disabled={busy || !code}>Confirmar e ligar</button>
    </form>}
    {user.twofa && <form class="flex flex-col gap-3" onSubmit={(e) => { e.preventDefault(); void run(() => factor('disable')); }}>
     <Field label="Senha atual" type="password" required autocomplete="current-password" value={password} onInput={(e: any) => setPassword(e.currentTarget.value)} />
     {(user.twofa === 'totp' || codeSent) && <Field label={user.twofa === 'totp' ? 'Código do app ou de recuperação' : 'Código recebido por e-mail'} autocomplete="one-time-code" required value={code} onInput={(e: any) => setCode(e.currentTarget.value)} />}
     <button class={go} disabled={busy || !password || ((user.twofa === 'totp' || codeSent) && !code)}>{user.twofa === 'email' && !codeSent ? 'Enviar código para desligar' : 'Desligar'}</button>
     {user.twofa === 'totp' && <button type="button" class={quiet} disabled={busy || !password} onClick={() => void run(() => factor('recovery'))}>Gerar novos códigos de recuperação</button>}
     <p class={hint}>Para trocar de método, desligue e ligue de novo.</p>
    </form>}
   </div>}

   {codes.length > 0 && <div class={card + ' flex flex-col gap-3'}>
    <p class="text-item text-foreground">Códigos de recuperação</p>
    <p class={hint}>Se perder o celular, cada um destes entra no lugar do código do app, uma vez. Guarde fora deste aparelho. Gerar novos invalida estes.</p>
    <pre class="whitespace-pre-wrap break-words font-mono text-item text-foreground">{codes.join('\n')}</pre>
    <button class={quiet} onClick={() => void run(async () => { await navigator.clipboard.writeText(codes.join('\n')); setInfo('Códigos copiados.'); })}>Copiar</button>
    <button class={go} onClick={() => setCodes([])}>Já guardei</button>
   </div>}
  </div>
  {error && <p role="alert" class="text-meta text-danger-soft-fg">{error}</p>}
  {info && <p role="status" class={hint}>{info}</p>}
 </section>;
}
