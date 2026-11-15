import assert from 'node:assert/strict';
import {followTrackSurface} from './autoteste-surface.mjs';
const pause=ms=>new Promise(r=>setTimeout(r,ms));
const track={id:'screen'},state={},frames=[];
function fake(play=()=>Promise.resolve()) {return {srcObject:{getVideoTracks:()=>[track]},play,listeners:{},addEventListener(e,cb){this.listeners[e]=cb;},removeEventListener(e){delete this.listeners[e];},callbacks:[],requestVideoFrameCallback(cb){this.callbacks.push(cb);},getVideoPlaybackQuality(){return {totalVideoFrames:this.total||0,droppedVideoFrames:0};},emit(n){this.total=n;this.callbacks.shift()?.(n,{presentedFrames:n});}};}
let video=fake(()=>new Promise(()=>{}));
const surface=followTrackSurface({state,track,find:()=>[video],timeout:180,poll:5,onFrame:(v,now,m)=>{frames.push(v);state.composed=surface.composed(m.presentedFrames);surface.next();}});
try {
 const ready=surface.ready();await pause(10);const old=video;let aborted=false;video=fake(()=>{if(aborted)return Promise.resolve();aborted=true;return Promise.reject(Object.assign(Error('reload'),{name:'AbortError'}));});await pause(15);while(video.callbacks.length>1)video.callbacks.shift()(0,{presentedFrames:999});video.emit(60);await ready;
 assert.equal(state.video,video,'replacement must follow track despite unresolved play');assert.equal(state.composed,60);
 old.emit(900);assert.equal(state.composed,60,'dead surface must not contaminate counters');
 const second=video;video=fake();await pause(15);video.emit(2);assert.equal(state.composed,62,'compositor count must survive replacement');assert.equal(surface.quality().totalVideoFrames,62);
 video.srcObject={getVideoTracks:()=>[track]};await pause(15);video.emit(999);assert.equal(state.composed,62,'pre-load callback must be ignored');video.emit(3);assert.equal(state.composed,65,'same element reload must rearm callback');
 // New load callbacks use a new generation: an old callback cannot advance it.
 assert.equal(state.video,video);video.listeners.emptied();video.emit(999);assert.equal(state.composed,65);video.emit(4);assert.equal(state.composed,69,'reload with identical stream must rearm callback');
 const began=Date.now();await assert.rejects(surface.deadline(new Promise(()=>{}),'hung-copy'),/hung-copy/);assert(Date.now()-began<500,'pixel await must obey its deadline');
} finally {surface.stop();}
const missingState={};const missing=followTrackSurface({state:missingState,track,find:()=>[],timeout:25,poll:5,onFrame(){}});
try{await assert.rejects(missing.ready(),/surface-(missing|first-frame)/);}finally{missing.stop();}
console.log('track surface replacement, hung play, stale callback, counters and deadlines passed');
