import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
const source=readFileSync(new URL('./autoteste-tela.mjs',import.meta.url),'utf8');
const start=source.indexOf('async function frame(now,metadata)');
assert(start>=0);
let end=source.indexOf('{',start)+1,depth=1;
while(depth){depth+=(source[end]==='{')-(source[end]==='}');++end;}
let registered=0,resolveCopy,closed=0;
const state={frames:[],flashes:[],surface:{next(){++registered;},composed(n){return n??null;},deadline(p){return p;}}},video={requestVideoFrameCallback(){++registered;}};
class Frame {
  displayWidth=1920;displayHeight=1080;copies=0;
  copyTo(bytes){if(++this.copies===1)return new Promise(resolve=>{resolveCopy=resolve;});bytes.set([255,0,255,255],32);return Promise.resolve();}
  close(){++closed;}
}
// Evaluate the exact production callback, replacing only the browser edges.
const callback=new Function('video','state','VideoFrame','return ('+source.slice(start,end)+')')(video,state,Frame);
const pending=callback(10,{mediaTime:1,width:1920,height:1080});
assert.equal(registered,1,'next callback must be registered before asynchronous pixels finish');
await callback(20,{mediaTime:2,width:1920,height:1080,presentedFrames:12,rtpTimestamp:90000});
assert.equal(state.composed,12,'compositor counter survives a busy pixel probe');
assert.equal(state.frames.at(-1).rtpTimestamp,90000,'retain RTP timestamp for cadence diagnosis');
assert.equal(state.frames.length,2,'busy pixel probe must not lose presentation count');
resolveCopy();await pending;
assert(state.noticeLeaked,'magenta transmission notice must be detected inside the sampled strip');
assert.equal(closed,1);
console.log('production pixel callback: nonblocking count and notice leak detected');
