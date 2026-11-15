import {createServer} from 'node:http';
import {spawn} from 'node:child_process';
import {createHash} from 'node:crypto';
import {resolve} from 'node:path';
import assert from 'node:assert/strict';
const bytes=Buffer.from('MZ synthetic update transport fixture');
const sha=createHash('sha256').update(bytes).digest('hex');
let foreign=0;
const other=createServer((req,res)=>{foreign++;res.end(bytes);});await new Promise(r=>other.listen(0,'127.0.0.1',r));
const server=createServer((req,res)=>{
  assert.equal(req.headers.cookie,'harmony_session='+'A'.repeat(43));
  if(req.url.includes('redirect')){res.writeHead(302,{Location:`http://127.0.0.1:${other.address().port}/stolen`});res.end();}
  else if(req.url.includes('truncated')){res.writeHead(200,{'Content-Length':bytes.length});res.write(bytes.subarray(0,4));res.destroy();}
  else {res.end(bytes);}
});
await new Promise(r=>server.listen(0,'127.0.0.1',r));
async function test(name,patch,expected){
 const release={version_code:27,size:bytes.length,sha256:sha,url:`/api/app/windows/harmony-27-${name}.exe`,...patch};
 const child=spawn(resolve('test-output/harmony-updater-tests.exe'),['--download'],{windowsHide:true,env:{...process.env,HARMONY_URL:`http://127.0.0.1:${server.address().port}`},stdio:['pipe','pipe','pipe']});
 let text='';child.stdout.on('data',b=>text+=b);child.stdin.end(JSON.stringify(release)+'\n');
 const code=await new Promise((r,j)=>{child.on('exit',r);child.on('error',j);});assert.equal(code,0);assert.equal(text.trim(),expected);console.log('PASS: update transport '+name);
}
try{
 await test('complete',{},'accepted');
 await test('wrong-hash',{sha256:'0'.repeat(64)},'rejected');
 await test('too-long',{size:bytes.length-1},'rejected');
 await test('truncated',{},'rejected');
 await test('redirect',{},'rejected');assert.equal(foreign,0,'redirect leaked request');
 console.log('PASS: redirect target received no request or session');
}finally{server.close();other.close();}
