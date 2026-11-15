import assert from 'node:assert/strict';
import {assertNoPreviousRun,killTree,assertRoundCanContinue} from './autoteste-processes.mjs';
import {resolve} from 'node:path';
const root=resolve('.'),path=resolve(root,'windows/app/out/harmony-nativo.exe');
await assert.rejects(assertNoPreviousRun(root,async()=>({stdout:JSON.stringify([{ProcessId:42,ExecutablePath:path}])})),/PID 42/);
await assertNoPreviousRun(root,async()=>({stdout:JSON.stringify([{ProcessId:43,ExecutablePath:resolve(root,'other/harmony-nativo.exe')}])}));
await assert.rejects(assertNoPreviousRun(root,async()=>({stdout:JSON.stringify([{ProcessId:44,ExecutablePath:null}])})),/PID 44/);
// The tasklist fast path (WMI can hang): none by name passes without WMI; a
// match by name still goes to WMI for the path.
const table=rows=>rows.map(([n,pid])=>`"${n}","${pid}","Console","1","1.000 K"`).join(String.fromCharCode(13,10));
let asked=[];await assertNoPreviousRun(root,async c=>{asked.push(c);if(c==='tasklist.exe')return {stdout:table([['System','4'],['explorer.exe','10']])};throw Error('WMI must not be asked');});
assert.deepEqual(asked,['tasklist.exe']);
await assert.rejects(assertNoPreviousRun(root,async c=>c==='tasklist.exe'?{stdout:table([['harmony-nativo.exe','47']])}:{stdout:JSON.stringify([{ProcessId:47,ExecutablePath:path}])}),/PID 47/);
await assert.rejects(assertNoPreviousRun(root,async c=>c==='tasklist.exe'?{stdout:table([['media-game.exe','48']])}:Promise.reject(Object.assign(Error(),{code:null}))),/Process inventory unavailable/);
let command,args;await killTree({pid:45,exitCode:null},async(c,a)=>{command=c;args=a;});
assert.equal(command,'taskkill.exe');assert.deepEqual(args,['/PID','45','/T','/F']);
await assert.rejects(killTree({pid:46,exitCode:null},async()=>{throw Object.assign(Error(),{code:5});}),/PID 46/);
console.log('previous-run refusal by path/PID and tree cleanup passed');

// A cleanup failure is recoverable only after a successful fresh inventory.
let inventories=0;
await assertRoundCanContinue({cleanup_failure:'Tree cleanup failed; PID 20504; code 255',stage:'browser_done',sync_windows:[{}],high_windows:[{}],native_samples:[{}]},root,async()=>{inventories++;return {stdout:table([['System','4']])};});
assert.equal(inventories,1);
await assert.rejects(assertRoundCanContinue({failure_step:'measuring',cleanup_failure:'failed'},root,async()=>{throw Error('must not inventory');}),/Incomplete autotest/);
await assert.rejects(assertRoundCanContinue({cleanup_failure:'failed'},root),/Incomplete autotest/);
await assert.rejects(assertRoundCanContinue({cleanup_failure:'failed',stage:'browser_done',sync_windows:[{}],high_windows:[{}],native_samples:[{}]},root,async c=>c==='tasklist.exe'?{stdout:table([['harmony-nativo.exe','20504']])}:{stdout:JSON.stringify([{ProcessId:20504,ExecutablePath:path}])}),/PID 20504/);
console.log('cleanup continuation requires complete result and fresh preflight passed');

// Keep the PowerShell caller wired to the tested continuation policy.
const {readFile}=await import('node:fs/promises');
const script=await readFile(new URL('./w4-round23-compare.ps1',import.meta.url),'utf8');
assert.match(script,/--check-result/);
assert.match(script,/--preflight/);
assert.doesNotMatch(script,/failure_step -or \$result\.cleanup_failure/);
console.log('comparison script uses guarded cleanup continuation passed');
