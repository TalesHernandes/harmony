// Focused real-UI updater checks. Every process, account and path is disposable.
import {spawn,execFileSync} from 'node:child_process';
import {readFileSync,writeFileSync,mkdirSync,existsSync,copyFileSync} from 'node:fs';
import {resolve,join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
import {verifyFixtureEmail} from '../testes/autoteste-email.mjs';
const repo=resolve(import.meta.dirname,'../../..');process.chdir(repo);
const output=resolve('windows/app/test-output');
const fixture=join(output,'updater-fixture-'+Date.now());
const packages=join(output,'packages'),published=join(fixture,'published');
mkdirSync(published,{recursive:true});mkdirSync(join(fixture,'downloads'));
writeFileSync(join(fixture,'.harmony-installer-test'),'isolated updater fixture');
process.env.TEMP=join(output,'temp');process.env.TMP=process.env.TEMP;mkdirSync(process.env.TEMP,{recursive:true});
process.env.GOCACHE=join(output,'go-cache');process.env.GOFLAGS='-buildvcs=false';
process.env.HARMONY_E2E_PORT='18199';process.env.HARMONY_APP_DIR=published;
const {start,stop,restart,BASE}=await import('../../../e2e-harmony/server.mjs');
const env={...process.env,HARMONY_URL:BASE,HARMONY_INSTALLER_TEST_ROOT:fixture,HARMONY_INSTALLER_PHOTO_PREFIX:'w7g-foto',HARMONY_LOG_FILE:join(fixture,'shell.log')};
const root=join(fixture,'programs/harmony'),stable=join(root,'harmony.exe'),meta=join(root,'harmony.next.exe.json');
const password=readFileSync('e2e-harmony/fixtures.ts','utf8').match(/export const PASSWORD = '([^']+)';/)[1];
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const json=file=>JSON.parse(readFileSync(file,'utf8').replace(/^\uFEFF/,''));
const hash=file=>createHash('sha256').update(readFileSync(file)).digest('hex');
const lines=[],metrics={};let watcher;
function pass(text){lines.push('PASS: '+text);console.log(lines.at(-1));}
function action(name,expected=126){
 const text=execFileSync('powershell.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',resolve('windows/app/instalar/fixture.ps1'),'-Action',name,'-Expected',String(expected)],
  {env,windowsHide:true,encoding:'utf8',input:name==='Login'?JSON.stringify({username:'updater_fixture',password})+'\n':undefined});
 if(name!=='UiState'&&text.trim()){lines.push(text.trim());console.log(text.trim());}return text.trim();
}
async function wait(label,predicate,ms=90000){
 const end=Date.now()+ms;
 while(Date.now()<end){try{if(await predicate())return;}catch(error){if(error.code!=='ENOENT'&&!(error instanceof SyntaxError))throw error;}await sleep(100);}
 throw Error('timeout: '+label);
}
const launch=(exe=stable,args=[])=>args.length?spawn(exe,args,{env,windowsHide:true,stdio:'ignore'}):spawn(join(packages,'user-launch-fixture.exe'),[exe],{env,windowsHide:true,stdio:'ignore'});
const publish=version=>copyFileSync(join(packages,`harmony-${version}-native.exe`),join(published,`harmony-${version}-native.exe`));
async function ui(title){await wait(title,()=>action('UiState').includes(title));}
async function removedCandidate(){await wait('candidate cleanup',()=>!existsSync(meta)&&!existsSync(join(root,'harmony.next.exe')));}
async function foreground(expected){await wait('native foreground',()=>{try{action('CheckForeground',expected);return true;}catch{return false;}});}
function clock(line){const m=line.match(/^(\d+):(\d+):(\d+)\.(\d+)/);return m?((+m[1]*60+ +m[2])*60+ +m[3])*1000+ +m[4]:null;}
try {
 for(const version of [126,127,128,129]){
  const file=join(packages,`harmony-${version}-native.exe`);
  assert.equal(readFileSync(file+'.test-only.sha256','utf8').trim().toLowerCase(),hash(file),'test-only fixture proof');
 }
 const {invites}=await start(1);
 const signup=await fetch(BASE+'/api/signup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code:invites[0],username:'updater_fixture',password,email:'updater@example.invalid'})});
 assert.equal(signup.status,201);await verifyFixtureEmail(signup,'updater@example.invalid',BASE);
 publish(127);
 watcher=spawn('powershell.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',resolve('windows/app/instalar/watch-update-ui.ps1'),'-Root',fixture],{env,windowsHide:true,stdio:'ignore'});
 await wait('UI watcher ready',()=>existsSync(join(fixture,'ui-watch.ready')));
 const download=join(fixture,'downloads/harmony-126-native.exe');copyFileSync(join(packages,'harmony-126-native.exe'),download);launch(download);
 await ui('Instalar o harmony');action('Primary');await ui('harmony instalado');action('Primary');
 await foreground(126);await sleep(3500);
 assert(!existsSync(meta),'opening without a session downloaded a candidate');
 assert(readFileSync(join(fixture,'shell.log'),'utf8').includes('update check deferred'),'initial no-session failure not observed');
 const previousLines=readFileSync(join(fixture,'shell.log'),'utf8').split(/\r?\n/).length-1;
 const submitted=Date.now();action('Login');
 await wait('download after live login',()=>existsSync(meta)&&json(meta).version_code===127,12000);
 metrics.login_to_download_ms=Date.now()-submitted;
 const loginLog=readFileSync(join(fixture,'shell.log'),'utf8').split(/\r?\n/).slice(previousLines);
 const signed=loginLog.find(line=>line.endsWith(' signed in')),query=loginLog.find(line=>line.endsWith(' update availability check'));
 assert(signed&&query,'native login/check diagnostic timestamps missing');metrics.signed_in_to_query_ms=(clock(query)-clock(signed)+86400000)%86400000;
 assert(metrics.signed_in_to_query_ms<2000,'login check did not run in seconds');
 pass('opening without a session then signing in through the native controls checks availability in seconds');
 action('StopNative');launch();
 await wait('127 installed and ready',()=>json(join(root,'install-result.json')).release===127);
 await foreground(127);await removedCandidate();
 await wait('compact update photo',()=>json(join(fixture,'update-ui.json')).frames.some(frame=>frame.title.includes('Atualizando')));
 pass('visible automatic update reports real steps, disappears on native readiness, and cleans the applied candidate');
 publish(128);action('StopNative');
 const failuresBefore=readFileSync(join(fixture,'shell.log'),'utf8').split('update check deferred').length;
 const outage=restart(6000);launch();await outage;
 const recovered=Date.now();
 await wait('short retry after network outage',()=>existsSync(meta)&&json(meta).version_code===128,12000);
 metrics.network_recovery_to_download_ms=Date.now()-recovered;
 assert(readFileSync(join(fixture,'shell.log'),'utf8').split('update check deferred').length>failuresBefore,'network failure was not observed');
 pass('real server outage fails a check and the updater retries at the accelerated short interval after recovery');
 action('StopNative');launch();
 await wait('128 rejected',()=>json(join(root,'failed-release.json')).version===128);
 await foreground(127);await removedCandidate();
 assert.equal(json(join(root,'native/current.json')).version,127);
 await wait('rollback photo',()=>json(join(fixture,'update-ui.json')).frames.some(frame=>frame.title.includes('Voltando')));
 await sleep(4000);assert(!existsSync(meta),'failed release downloaded again');
 pass('failed native startup announces the return before opening 127, cleans the candidate, and suppresses the failed release');
 publish(129);writeFileSync(join(fixture,'ui-watch.tray'),'tray');action('StopNative');launch(stable,['--tray']);
 await wait('129 staged while hidden',()=>existsSync(meta)&&json(meta).version_code===129);
 action('StopNative');launch(stable,['--tray']);
 await wait('129 applied hidden',()=>json(join(root,'install-result.json')).release===129);
 await removedCandidate();action('CheckHidden',129);await sleep(1000);
 const frames=json(join(fixture,'update-ui.json'));assert.equal(frames.tray_visible_frames,0,'tray update revealed an installer frame');
 for(const frame of frames.frames){
  assert(frame.height/frame.dpi*96<=320,'automatic frame is not compact');
  if(frame.title.includes('Atualizando'))assert(frame.stage<4,'update frame remained visible during post-readiness registration');
  if(frames.secondary_bounds){const b=frames.secondary_bounds;assert(frame.left>=b.left&&frame.top>=b.top&&frame.left+frame.width<=b.right&&frame.top+frame.height<=b.bottom,'frame was not placed on the second monitor');}
 }
 metrics.ui=frames;pass('startup/tray applies a newer version without showing an update or native window');
 launch();await foreground(129);action('Uninstall');await ui('Desinstalar');action('Primary');await ui('harmony desinstalado');action('CheckRemoved');action('Primary');
 pass('focused updater installation, live login, network retry, visible/hidden apply, rollback and uninstall complete');
}catch(error){lines.push('FAIL: '+error.message);console.error(lines.at(-1));process.exitCode=1;}
finally {
 writeFileSync(join(fixture,'ui-watch.stop'),'stop');
 if(watcher?.exitCode==null){await Promise.race([new Promise(r=>watcher.once('exit',r)),sleep(5000)]);if(watcher.exitCode==null)watcher.kill();}
 if(existsSync(join(root,'.harmony-installer'))){try{action('Cleanup');}catch{lines.push('FAIL: isolated updater cleanup');process.exitCode=1;}}
 stop();writeFileSync(join(output,'w7g/updater-e2e.log'),lines.join('\n')+'\n');writeFileSync(join(output,'w7g/updater-e2e-metrics.json'),JSON.stringify(metrics,null,2));
}
