import { useState } from 'preact/hooks';
import { changePreference, isBlocked, preferences, usePreferences } from './blocks';
import { message } from './api';
import { Confirm } from './Confirm';
import { Icon } from './Icon';
export function PersonalActions({username}: {username: string}) {
 usePreferences();
 const [asking,setAsking] = useState(false);
 const [error,setError] = useState('');
 const [busy,setBusy] = useState(false);
 const p = preferences().find(p => p.username === username);
 async function change(block: boolean, enabled: boolean) { setBusy(true); setError(''); try { await changePreference(username,block,enabled); setAsking(false); } catch(e) {setError(message(e));} finally {setBusy(false);} }
 const row='flex h-11 w-full items-center gap-3 rounded-control bg-default px-3 text-item text-foreground hover:bg-default-hover disabled:opacity-50';
 return <div class="flex flex-col gap-2">
 <button class={row} disabled={busy} onClick={() => void change(false,!p?.muted)}><Icon name="microphone-slash" size={20} />{p?.muted ? 'Tirar o silêncio' : 'Silenciar'}</button>
 <button class={row} disabled={busy} onClick={() => isBlocked(username) ? void change(true,false) : setAsking(true)}><Icon name="ban" size={20} />{isBlocked(username) ? 'Desbloquear' : 'Bloquear'}</button>
 {error && <p role="alert" class="text-meta text-danger-soft-fg">{error}</p>}
 {asking && <Confirm question="Bloquear esta pessoa?" description="Você deixa de ouvir a voz e a tela dela, as mensagens ficam recolhidas e não haverá novas mensagens privadas nem chamadas entre vocês. Ela não será avisada." actionLabel="Bloquear" actionIcon="ban" onConfirm={() => {if(!busy) void change(true,true);}} onCancel={() => setAsking(false)}>@{username}</Confirm>}
 </div>;
}
export function PersonalList() {
 const items=usePreferences();
 return <section class="flex flex-col gap-3"><h3 class="text-label uppercase text-foreground-muted">Pessoas silenciadas e bloqueadas</h3>
 {items.length === 0 && <p class="text-meta text-foreground-muted">Você não silenciou nem bloqueou ninguém.</p>}
 {items.map(p => <div key={p.username} class="flex flex-col gap-2 rounded-control bg-surface-2 p-3"><p class="text-item text-foreground">@{p.username}</p><PersonalActions username={p.username}/></div>)}
 </section>;
}
export function BlockedConversation({username}: {username: string}) {
 const [error,setError]=useState('');
 return <div class="flex flex-col gap-2 p-4 text-item text-foreground-muted" data-blocked-conversation>Você bloqueou esta pessoa. Desbloqueie para conversar.
 <button class="h-11 rounded-control bg-default px-4 text-foreground hover:bg-default-hover" onClick={() => void changePreference(username,true,false).catch(e => setError(message(e)))}>Desbloquear</button>{error && <p role="alert">{error}</p>}</div>;
}
