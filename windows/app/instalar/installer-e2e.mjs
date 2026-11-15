// Real Go server + real native UI. Disposable installation and registry only.
import {spawn, execFileSync} from 'node:child_process';
import {readFileSync,writeFileSync,mkdirSync,existsSync,copyFileSync,readdirSync,renameSync,unlinkSync} from 'node:fs';
import {resolve,join} from 'node:path';
import {createHash} from 'node:crypto';
import assert from 'node:assert/strict';
if(process.argv.slice(2).some(arg=>arg!=='--capture')) throw Error('Only --capture is supported.');
const repo=resolve(import.meta.dirname,'../../..');
process.chdir(repo);
const output=resolve('windows/app/test-output');
const fixture=join(output,'installer-fixture-'+Date.now());
const packages=join(output,'packages');
const downloads=join(fixture,'downloads');mkdirSync(downloads,{recursive:true});
const userLaunch=exe=>spawn(join(packages,'user-launch-fixture.exe'),[exe],{env,stdio:'ignore',windowsHide:true});
let downloadNumber=0;
const downloaded=version=>{const path=join(downloads,`harmony-${version}-native-${++downloadNumber}.exe`);copyFileSync(join(packages,`harmony-${version}-native.exe`),path);return userLaunch(path);};
const published=join(fixture,'published');
mkdirSync(fixture,{recursive:true});mkdirSync(published,{recursive:true});
writeFileSync(join(fixture,'.harmony-installer-test'),'W7 disposable test root');
// The real root sits under %USERPROFILE%\AppData, which is hidden; Get-Item
// without -Force cannot see it; exercise the production ancestor attributes.
mkdirSync(join(fixture,'programs'),{recursive:true});execFileSync('attrib',['+h',join(fixture,'programs')]);
const temp=join(output,'w7-temp');mkdirSync(temp,{recursive:true});
process.env.TEMP=temp;process.env.TMP=temp;
process.env.HARMONY_E2E_PORT='18197';process.env.HARMONY_APP_DIR=published;
const {start,stop,BASE}=await import('../../../e2e-harmony/server.mjs');
const env={...process.env,HARMONY_INSTALLER_TEST_ROOT:fixture,HARMONY_URL:BASE,HARMONY_UPDATE_DELAY_MS:'1500',HARMONY_INSTALLER_PHOTO_PREFIX:process.env.HARMONY_INSTALLER_PHOTO_PREFIX||'w7g-foto',HARMONY_LOG_FILE:join(fixture,'shell.log')};
const defaultRoot=join(fixture,'programs/harmony');
let root=defaultRoot,stable=join(root,'harmony.exe'),pointer=join(root,'native/current.json');
const password=readFileSync('e2e-harmony/fixtures.ts','utf8').match(/export const PASSWORD = '([^']+)';/)[1];
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const json=file=>JSON.parse(readFileSync(file,'utf8').replace(/^\uFEFF/,''));
const hash=file=>createHash('sha256').update(readFileSync(file)).digest('hex');
const lines=[];
function pass(line){lines.push('PASS: '+line);console.log(lines.at(-1));}
async function wait(label,predicate,ms=90000){const end=Date.now()+ms;while(Date.now()<end){if(await predicate())return;await sleep(250);}throw Error('timeout: '+label);}
function fixtureAction(action,expected=126,destination){
 const out=execFileSync('powershell.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',resolve('windows/app/instalar/fixture.ps1'),'-Action',action,'-Expected',String(expected),...(destination?['-Destination',destination]:[])],{env,encoding:'utf8',windowsHide:true});
 if(action!=='UiState'&&out.trim()){lines.push(out.trim());console.log(out.trim());}return out.trim();
}
const capturedSteps=new Set();
async function ui(text){await wait('installer UI: '+text,()=>{try{
 const state=fixtureAction('UiState');
 if(state.includes('Instalando')||state.includes('Desinstalando')) {
  for(const name of readdirSync(join(fixture,'logs')).filter(n=>/^progress-\d+\.json$/.test(n))) {
   const record=json(join(fixture,'logs',name));const key=state+record.step;
   if(!capturedSteps.has(key)) {fixtureAction('Capture');capturedSteps.add(key);}
  }
 }
 return state.includes(text);
}catch{return false;}},180000);fixtureAction('Capture');}
function launch(){return userLaunch(stable);}
function publish(version){const source=join(packages,`harmony-${version}-native.exe`);assert(existsSync(source),'package missing');copyFileSync(source,join(published,`harmony-${version}-native.exe`));}

try{
 for(const name of [126,127,128].map(v=>`harmony-${v}-native.exe`)) {
   const file=join(packages,name);
   assert.equal(readFileSync(file+'.test-only.sha256','utf8').trim().toLowerCase(),hash(file),'rebuild test-only fixtures before launching');
 }
 assert(!existsSync(pointer),'use a new fixture root after a prior run');
 const {invites}=await start(2);
 const signup=await fetch(BASE+'/api/signup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code:invites[0],username:'installer_fixture',password,email:'w7@example.invalid'})});
 assert.equal(signup.status,201,'test account signup');
 const signupCookie=signup.headers.get('set-cookie')?.split(';')[0];
 let code;
 await wait('fixture verification mail',()=>{
   for(const file of readdirSync(process.env.HARMONY_E2E_MAIL_DIR).filter(f=>f.endsWith('.eml'))){
     const mail=readFileSync(join(process.env.HARMONY_E2E_MAIL_DIR,file),'utf8');
     if(mail.includes('To: <w7@example.invalid>')) code=mail.match(/\b\d{6}\b/)?.[0];
   }return !!code;
 });
 const verified=await fetch(BASE+'/api/me/email/verify',{method:'POST',headers:{Cookie:signupCookie,'Content-Type':'application/json'},body:JSON.stringify({code})});
 assert.equal(verified.status,200,'fixture email verification');code=undefined;

   downloaded(128);await ui('Instalar o harmony');
   const occupied=join(fixture,'occupied');mkdirSync(occupied);writeFileSync(join(occupied,'keep.txt'),'untouched');
   const alias=join(fixture,'junction');execFileSync('powershell.exe',['-NoProfile','-Command',`New-Item -ItemType Junction -Path '${alias}' -Target '${occupied}' | Out-Null`],{windowsHide:true});
   for(const path of [process.env.ProgramFiles,process.env.windir,resolve(process.env.SystemDrive+'\\'),occupied,alias,'\\\\server\\share',join(occupied,'keep.txt')]) {
     fixtureAction('SelectLocation',126,path);
     const result=json(join(fixture,'location-result.json'));assert(result.error,'unsafe location accepted: '+path);assert.equal(result.root,defaultRoot);
     fixtureAction('Capture');
   }
   assert.equal(readFileSync(join(occupied,'keep.txt'),'utf8'),'untouched');
   execFileSync('powershell.exe',['-NoProfile','-Command',`[IO.Directory]::Delete('${alias}')`],{windowsHide:true});
   pass('picker refuses protected directories, disk root, occupied directory, junction, network path and file without changing destination');
   fixtureAction('Secondary');await sleep(1500);
   assert(!existsSync(root),'cancel created an install root');pass('cancel leaves the installation absent');
   downloaded(128);await ui('Instalar o harmony');fixtureAction('Primary');await ui('Não foi possível instalar');
   fixtureAction('CheckRemoved');fixtureAction('Primary');await sleep(1500);
   pass('first-install failure after copying leaves no tree, shortcuts, Run or uninstall registration');
   env.HARMONY_INSTALLER_FAIL_PHASE='shortcuts';
   downloaded(126);await ui('Instalar o harmony');fixtureAction('Primary');await ui('Não foi possível instalar');
   fixtureAction('CheckRemoved');fixtureAction('Primary');await sleep(1500);delete env.HARMONY_INSTALLER_FAIL_PHASE;
   pass('injected failure before shortcut creation also removes every first-install artifact');
   fixtureAction('SeedBroken');
   downloaded(126);await ui('Instalar o harmony');fixtureAction('Primary');await ui('harmony instalado');fixtureAction('Primary');
   await wait('fresh native login readiness',()=>existsSync(join(root,'install-result.json')),120000);
   const result=json(join(root,'install-result.json'));
   assert.equal(result.native_ready,false);assert.equal(result.authenticated_ui,false);
   assert(!existsSync(join(fixture,'data/native/session.dpapi')),'fresh install imported a session');
   assert.equal(hash(stable),hash(join(packages,'harmony-126-native.exe')));
   fixtureAction('Check',126);fixtureAction('CheckForeground',126);
   const noContext={...env};delete noContext.HARMONY_INSTALLER_TEST_ROOT;
   const refused=spawn(join(root,'native/app/harmony-nativo.exe'),[],{env:noContext,stdio:'ignore',windowsHide:true});
   assert.equal(await new Promise(r=>refused.once('exit',r)),2,'fixture native accepted missing context');
   let hostRefused=false;
   try { execFileSync('powershell.exe',['-NoProfile','-ExecutionPolicy','Bypass','-File',join(root,'uninstall-native.ps1'),'-Root',root,'-Remove'],{env:noContext,stdio:'ignore',windowsHide:true}); }
   catch {hostRefused=true;}
   assert(hostRefused,'fixture uninstaller accepted missing context');fixtureAction('Check',126);
   pass('installed native and durable uninstaller refuse missing test context before touching identity');
   pass('fresh bridge from test Downloads installed stable launcher and opened native login without a session');
   if(process.argv.includes('--capture')) { console.log('CAPTURE: fresh native login (45 seconds)');await sleep(45000); }
   downloaded(126);await ui('harmony já está instalado');fixtureAction('Primary');await sleep(5000);fixtureAction('CheckRunning',126);fixtureAction('CheckForeground',126);
   pass('same-code installer offers Open and activates the installed native without a duplicate');
   env.HARMONY_INSTALLER_FAIL_PHASE='shortcuts';
   downloaded(126);await ui('harmony já está instalado');fixtureAction('Secondary');await ui('Não foi possível instalar');fixtureAction('Primary');
   delete env.HARMONY_INSTALLER_FAIL_PHASE;
   assert.equal(hash(stable),hash(join(packages,'harmony-126-native.exe')));
   fixtureAction('Check',126);fixtureAction('CheckRunning',126);
   launch();await sleep(5000);fixtureAction('CheckForeground',126);
   pass('failed Reinstall restores the working version, launcher, shortcuts, registration and startup');
   downloaded(126);await ui('harmony já está instalado');fixtureAction('Secondary');await ui('harmony instalado');fixtureAction('Primary');
   await sleep(5000);fixtureAction('Check',126);fixtureAction('CheckRunning',126);fixtureAction('CheckForeground',126);
   pass('same-code Reinstall replaces files transactionally and restores Windows integration');
   downloaded(127);await ui('Atualizar');fixtureAction('Primary');await ui('harmony instalado');fixtureAction('Primary');
   await sleep(5000);fixtureAction('Check',127);fixtureAction('CheckForeground',127);
   assert.equal(json(pointer).version,127);pass('newer downloaded installer updates the native without requiring a session');
   // Return to N through the registered uninstaller to exercise automatic N -> N+1 separately.
   fixtureAction('Uninstall');await ui('Desinstalar');fixtureAction('Secondary');await sleep(1000);fixtureAction('Check',127);
   fixtureAction('Uninstall');await ui('Desinstalar');fixtureAction('Primary');await ui('harmony desinstalado');fixtureAction('CheckRemoved');fixtureAction('Primary');await sleep(1500);
   const customParent=join(fixture,'custom empty parent');mkdirSync(customParent);
   downloaded(126);await ui('Instalar o harmony');fixtureAction('SelectLocation',126,customParent);
   const chosen=json(join(fixture,'location-result.json'));assert.equal(chosen.error,'');
   root=join(customParent,'harmony');stable=join(root,'harmony.exe');pointer=join(root,'native/current.json');env.HARMONY_INSTALLER_EXPECTED_ROOT=root;
   assert.equal(chosen.root,root);assert(!existsSync(defaultRoot));fixtureAction('Capture');
   fixtureAction('Primary');await ui('harmony instalado');fixtureAction('Primary');await sleep(4000);
   fixtureAction('Check',126);pass('empty custom parent installs into its harmony child and leaves default destination absent');
   downloaded(126);await ui('harmony já está instalado');fixtureAction('Secondary');await ui('harmony instalado');fixtureAction('Primary');await sleep(4000);
   fixtureAction('Check',126);assert(!existsSync(defaultRoot));pass('downloaded same-code Reinstall finds the custom installation through InstallLocation');
   fixtureAction('StopNative');
   spawn(stable,['--tray'],{env,stdio:'ignore',windowsHide:true});
   await sleep(7000);fixtureAction('CheckHidden',126);
   // Starting with Windows must not reveal an already running tray instance.
   spawn(stable,['--tray'],{env,stdio:'ignore',windowsHide:true});
   await sleep(5000);fixtureAction('CheckHidden',126);
   launch();await sleep(5000);fixtureAction('CheckForeground',126);
   fixtureAction('StopNative');
   const writer=spawn(join(packages,'session-fixture.exe'),[],{env,stdio:['pipe','ignore','ignore'],windowsHide:true});
   writer.stdin.end(signupCookie+'\n');
   assert.equal(await new Promise(r=>writer.once('exit',r)),0,'test session writer');
   pass('fixture session verified on loopback and saved through native DPAPI');
 publish(127);
 // The first native check happens two seconds after opening in this fixture.
 // Reopen after publication so the check definitely sees the next release.
 fixtureAction('StopNative');launch();
 await wait('native updater verified N+1',()=>existsSync(join(root,'harmony.next.exe.json'))&&json(join(root,'harmony.next.exe.json')).version_code===127,120000);
 fixtureAction('StopNative');unlinkSync(join(fixture,'data/native/session.dpapi'));launch();
 await wait('N+1 ready',()=>json(join(root,'install-result.json')).release===127,120000);
 assert.equal(json(join(root,'install-result.json')).login_ui,true);
 assert.equal(json(pointer).version,127);assert(existsSync(join(root,'native/app-126/harmony-nativo.exe')));fixtureAction('Check',127);fixtureAction('CheckForeground',127);
 assert.equal(hash(stable),hash(join(packages,'harmony-127-native.exe')),'stable bridge did not update');
 await wait('successful update candidate cleanup',()=>!existsSync(join(root,'harmony.next.exe'))&&!existsSync(join(root,'harmony.next.exe.json')));
 pass('confirmed update removes harmony.next.exe and its metadata after the applying process exits');
 pass('native automatic download N → N+1 accepts login after session removal and retains previous version');
 fixtureAction('StopNative');
 const restoreWriter=spawn(join(packages,'session-fixture.exe'),[],{env,stdio:['pipe','ignore','ignore'],windowsHide:true});
 restoreWriter.stdin.end(signupCookie+'\n');assert.equal(await new Promise(r=>restoreWriter.once('exit',r)),0,'restore fixture session for next download');
 publish(128);fixtureAction('StopNative');launch();
 await wait('broken release downloaded',()=>existsSync(join(root,'harmony.next.exe.json'))&&json(join(root,'harmony.next.exe.json')).version_code===128,120000);
 fixtureAction('StopNative');launch();
 await wait('broken release rollback',()=>existsSync(join(root,'failed-release.json'))&&json(join(root,'failed-release.json')).version===128,120000);
 await wait('previous native after rollback notice',()=>{try{fixtureAction('CheckForeground',127);return true;}catch{return false;}});
 assert.equal(json(pointer).version,127);fixtureAction('Check',127);fixtureAction('CheckForeground',127);
 await wait('rolled back candidate cleanup',()=>!existsSync(join(root,'harmony.next.exe'))&&!existsSync(join(root,'harmony.next.exe.json')));
 pass('failed update removes the candidate and metadata while retaining failed-release.json');
 pass('release 128 failed to open: current.json and native registration returned to 127');
 if(process.argv.includes('--capture')) { console.log('CAPTURE: rollback error window (20 seconds)');await sleep(20000); }
 fixtureAction('Uninstall');await ui('Desinstalar');fixtureAction('Primary');await ui('harmony desinstalado');fixtureAction('CheckRemoved');fixtureAction('Primary');
 assert(!existsSync(defaultRoot));assert(!existsSync(root));assert.equal(readdirSync(customParent).length,0);
 pass('custom automatic N → N+1, rollback and registered uninstall leave custom parent empty and default destination absent');
 pass('uninstall confirmation removes tree, shortcuts, Run and Installed Apps entry');
 pass('fresh installation, automatic upgrade, rollback and uninstall completed');
}catch(error){lines.push('FAIL: '+error.message);console.error(lines.at(-1));process.exitCode=1;}
finally{
 if(existsSync(join(root,'.harmony-installer'))) {
   try{fixtureAction('Cleanup');}catch{lines.push('FAIL: isolated fixture cleanup failed');process.exitCode=1;}
 }
 stop();writeFileSync(join(output,'w7g-e2e.log'),lines.join('\n')+'\n');
}
