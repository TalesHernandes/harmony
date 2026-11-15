import assert from 'node:assert/strict';
import {spawn,execFile} from 'node:child_process';
import {promisify} from 'node:util';
import {resolve} from 'node:path';
const run=promisify(execFile),exe=resolve(process.argv[2]);
const parent=spawn(exe,['--parent'],{windowsHide:true,stdio:['pipe','pipe','pipe']});
parent.stdin.on('error',()=>{});
let ids=[];
try {
 const line=await new Promise((resolve,reject)=>{let text='';const timer=setTimeout(()=>reject(Error('guard fixture readiness timeout')),5000);parent.stdout.on('data',chunk=>{text+=chunk;if(text.includes('\n')){clearTimeout(timer);resolve(text.trim());}});});
 ids=line.split(' ').map(Number);assert.equal(ids.length,2);
 const watcher=spawn(exe,['--watch',...ids.map(String)],{windowsHide:true,stdio:['ignore','pipe','pipe']});
 const exited=new Promise(resolve=>watcher.once('exit',resolve));
 await new Promise((resolve,reject)=>{const timer=setTimeout(()=>reject(Error('watcher readiness timeout')),3000);watcher.stdout.once('data',()=>{clearTimeout(timer);resolve();});});
 parent.stdin.write('exit');
 assert.equal(await exited,0,'parent disappearance must kill fixture and its child');
 console.log('real Windows job kills fixture and child when parent disappears');
} finally {
 parent.stdin.end('exit');
 for(const pid of [parent.pid,...ids])try{await run('taskkill.exe',['/PID',String(pid),'/T','/F'],{windowsHide:true});}catch{}
}
