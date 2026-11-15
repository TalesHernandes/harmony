import { mkdirSync } from 'node:fs';
import { test, expect, Person } from './fixtures';
import type { Page } from '@playwright/test';

async function request(page: Page, method: string, path: string, body?: unknown) {
 return page.evaluate(async ([method,path,body]) => {
  const r=await fetch(path,{method,headers:{'Content-Type':'application/json'},body:body===undefined?undefined:JSON.stringify(body)});
  return {status:r.status, body:await r.json().catch(()=>null)};
 },[method,path,body] as const);
}
async function profile(p: Person, who: string) {
 if (!(await p.page.locator('aside').isVisible())) await p.page.getByRole('button',{name:'pessoas',exact:true}).first().click();
 await p.page.locator(`[data-person="${who}"]`).last().click();
 await expect(p.page.getByRole('dialog').first()).toBeVisible();
}
const photos=process.env.D13_PHOTOS;
async function photo(page: Page,name: string) {if(photos){mkdirSync(photos,{recursive:true});await page.screenshot({path:`${photos}/web-${name}.png`});}}

test('personal lists follow own sessions, collapse one message, and explain blocked DMs', async ({browser}) => {
 const ana=await Person.create(browser,'d13ana');
 const bob=await Person.create(browser,'d13bob');
 const other=await Person.signIn(browser,ana.username);
 const channels=(await request(ana.page,'GET','/api/channels')).body.channels;
 const ch=channels.find((c:any)=>c.kind==='text').id;
 const sent=await request(bob.page,'POST',`/api/channels/${ch}/messages`,{body:'D13 private reveal fixture'});
 const id=sent.body.id;
 await profile(ana,bob.username);
 await photo(ana.page,'perfil');
 await ana.page.getByRole('button',{name:'Silenciar',exact:true}).click();
 await expect(ana.page.getByRole('button',{name:'Tirar o silêncio',exact:true})).toBeVisible();
 await profile(other,bob.username);
 await expect(other.page.getByRole('button',{name:'Tirar o silêncio',exact:true})).toBeVisible();
 await ana.page.getByRole('button',{name:'Bloquear',exact:true}).click();
 await expect(ana.page.locator('[data-confirm]')).toBeVisible();
 await photo(ana.page,'confirmacao');
 await ana.page.locator('[data-confirm-yes]').click();
 await expect(ana.page.getByRole('button',{name:'Desbloquear',exact:true})).toBeVisible();
 await expect(other.page.getByRole('button',{name:'Desbloquear',exact:true})).toBeVisible();
 expect((await request(bob.page,'GET','/api/me/blocks')).body.items).toEqual([]);
 await ana.page.keyboard.press('Escape');
 const folded=ana.page.locator(`[data-blocked-message="${id}"]`);
 await expect(folded).toContainText('Mensagem de pessoa bloqueada');
 await photo(ana.page,'mensagem-recolhida');
 await folded.getByRole('button',{name:'Mostrar',exact:true}).click();
 await expect(ana.page.locator(`#msg-${id}`)).toContainText('D13 private reveal fixture');
 await photo(ana.page,'mensagem-aberta');
 await other.page.keyboard.press('Escape');
 await expect(other.page.locator(`[data-blocked-message="${id}"]`)).toBeVisible();
 const dm=(await request(ana.page,'POST','/api/dms',{username:bob.username})).body;
 await ana.openMessages();
 await ana.page.locator('nav').getByRole('button').filter({hasText:bob.username}).first().click();
 await expect(ana.page.locator('[data-blocked-conversation]')).toBeVisible();
 await photo(ana.page,'privado-bloqueado');
 const failed=await request(bob.page,'POST',`/api/channels/${dm.id}/messages`,{body:'refused'});
 expect(failed).toEqual({status:409,body:{error:'delivery_failed'}});
 await ana.page.locator('button[aria-label="ajustes"]').first().click();
 await ana.page.locator('[data-tab="conta"]').click();
 await expect(ana.page.getByText('Pessoas silenciadas e bloqueadas',{exact:true})).toBeVisible();
 await photo(ana.page,'ajustes');
 await ana.page.locator('[data-settings]').getByRole('button',{name:'Desbloquear',exact:true}).click();
 await ana.page.locator('[data-settings]').getByRole('button',{name:'Tirar o silêncio',exact:true}).click();
 await expect(ana.page.getByText('Você não silenciou nem bloqueou ninguém.',{exact:true})).toBeVisible();
 await photo(ana.page,'ajustes-vazio');
 await other.page.reload();
 expect((await request(other.page,'GET','/api/me/blocks')).body.items).toEqual([]);
 await ana.close();await bob.close();await other.close();
});

test('personal silence only gates microphone, block gates screen too, and saved volumes survive', async ({browser})=>{
 const ana=await Person.create(browser,'d13voiceana');
 const bob=await Person.create(browser,'d13voicebob');
 await ana.joinVoice(); await bob.joinVoice(); await bob.shareScreen(); await ana.watchScreen();
 await ana.page.evaluate(who=>{const v=(window as any).__harmonyVoice;v.setVolume(who,'mic',0.4);v.setVolume(who,'screen_audio',0.7);},bob.username);
 async function levels() {return ana.page.evaluate(who=>{const v=(window as any).__harmonyVoice;const out:Record<string,number[]>={};for(const [mid,i] of v.info){if(i.username===who&&['mic','screen_audio'].includes(i.source)){out[i.source]??=[];out[i.source].push(v.gains.get(mid)?.gain.value??-1);}}return out;},bob.username);}
 await expect.poll(levels).toEqual({mic:[expect.closeTo(0.4,5)],screen_audio:[expect.closeTo(0.7,5)]});
 await request(ana.page,'PUT',`/api/me/mutes/${bob.username}`);
 await expect.poll(levels).toEqual({mic:[0],screen_audio:[expect.closeTo(0.7,5)]});
 await expect(ana.page.locator('[data-person-muted]').first()).toBeVisible();
 await photo(ana.page,'vaga-silenciada');
 await request(ana.page,'PUT',`/api/me/blocks/${bob.username}`);
 await expect.poll(levels).toEqual({mic:[0],screen_audio:[0]});
 await request(ana.page,'DELETE',`/api/me/blocks/${bob.username}`);
 await expect.poll(levels).toEqual({mic:[0],screen_audio:[expect.closeTo(0.7,5)]});
 await request(ana.page,'DELETE',`/api/me/mutes/${bob.username}`);
 await expect.poll(levels).toEqual({mic:[expect.closeTo(0.4,5)],screen_audio:[expect.closeTo(0.7,5)]});
 await ana.close();await bob.close();
});

test('blocked channel messages generate no client unread, mention, or notification cue',async({browser})=>{
 const ana=await Person.create(browser,'d13quietana');const bob=await Person.create(browser,'d13quietbob');
 const owner=await Person.signIn(browser,'dono');
 const c=(await request(owner.page,'POST','/api/channels',{name:'d13 quiet',kind:'text'})).body;
 await request(ana.page,'PUT',`/api/me/blocks/${bob.username}`);
 await expect.poll(async()=> (await request(ana.page,'GET','/api/me/blocks')).body.items[0]?.blocked).toBe(true);
 await request(bob.page,'POST',`/api/channels/${c.id}/messages`,{body:'@'+ana.username+' ignored'});
 await expect.poll(async()=> (await request(ana.page,'GET','/api/channels')).body.channels.find((x:any)=>x.id===c.id)?.unread).toBe(0);
 const button=ana.page.getByRole('button',{name:'#'+c.name,exact:true});
 await expect(button).toBeVisible();
 await expect(button).not.toHaveClass(/font-semibold/);
 expect((await request(ana.page,'GET','/api/notifications/pending')).body.items).toEqual([]);
 // The server is shared: a channel left with a message is an unread badge on every account made later.
 await request(owner.page,'DELETE',`/api/channels/${c.id}`);
 await ana.close();await bob.close();await owner.close();
});
