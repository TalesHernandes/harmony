import {execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {readFile} from 'node:fs/promises';
import {resolve} from 'node:path';
const run=promisify(execFile);
export function previousRuns(processes,root) {
  const expected=[resolve(root,'windows/app/out/harmony-nativo.exe'),resolve(root,'windows/app/queixas/w4/media-game.exe')].map(p=>p.toLowerCase());
  return processes.filter(p=>expected.includes(String(p.ExecutablePath).toLowerCase()));
}
// WMI can hang for minutes on this machine (10/10/2026: 21 min for one query).
// tasklist does not use it. With no process of either name at all, nothing can
// hold the camera; any match still needs WMI's path, so it still refuses.
async function noneByName(execute) {
  const {stdout}=await execute('tasklist.exe',['/FO','CSV','/NH'],{timeout:10000,windowsHide:true});
  // Only a real process table counts: a row is "image","pid",...
  if(!/^"[^"]+","\d+",/m.test(stdout))return false;
  return !/^"(harmony-nativo|media-game)\.exe"/im.test(stdout);
}
export async function assertNoPreviousRun(root,execute=run) {
  try{if(await noneByName(execute))return;}catch{}
  let stdout;try{({stdout}=await execute('powershell.exe',['-NoProfile','-Command',"$ErrorActionPreference='Stop'; ConvertTo-Json -Compress -InputObject @(Get-CimInstance Win32_Process -Filter \"Name='harmony-nativo.exe' OR Name='media-game.exe'\" | Select-Object ProcessId,ExecutablePath)"],{timeout:10000,windowsHide:true}));}catch(error){throw Error('Process inventory unavailable; PID não encontrado; code '+error.code);}
  const parsed=JSON.parse(stdout||'[]'),processes=Array.isArray(parsed)?parsed:[parsed];
  // Missing paths cannot safely establish that no prior run holds the camera.
  if(processes.some(p=>!p.ExecutablePath))throw Error('Previous process path: não encontrado; PID '+processes.filter(p=>!p.ExecutablePath).map(p=>p.ProcessId).join(', '));
  const live=previousRuns(processes,root);
  if(live.length)throw Error('Previous W4 run alive; PID '+live.map(p=>p.ProcessId).join(', '));
}
export async function killTree(child,execute=run) {
  if(!child?.pid)return;
  try {await execute('taskkill.exe',['/PID',String(child.pid),'/T','/F'],{timeout:10000,windowsHide:true});}
  catch(error){if(child.exitCode==null)throw Error('Tree cleanup failed; PID '+child.pid+'; code '+error.code+'; detail '+String(error.stderr||error.stdout||error.message||'não encontrado').trim());}
}

export async function assertRoundCanContinue(result,root,execute=run) {
  if(result.failure_step||result.failure_kind||result.stage!=='browser_done'||!result.sync_windows?.length||!result.high_windows?.length||!result.native_samples?.length)throw Error('Incomplete autotest; comparison stopped');
  if(result.cleanup_failure)await assertNoPreviousRun(root,execute);
}
if(typeof process!=='undefined') {
  if(process.argv.includes('--preflight'))await assertNoPreviousRun(resolve('.'));
  const resultIndex=process.argv.indexOf('--check-result');
  if(resultIndex>=0)await assertRoundCanContinue(JSON.parse(await readFile(process.argv[resultIndex+1],'utf8')),resolve('.'));
}
