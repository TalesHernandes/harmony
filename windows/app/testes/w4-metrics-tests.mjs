import assert from 'node:assert/strict';
import {highWindows,cameraGrowing,syncWindows,rates} from './autoteste-metrics.mjs';
function fixture(tail=60) {return Array.from({length:181},(_,i)=>({at:i*1000,elapsed:i,phase:i<60?'high-initial':i<120?'low':'high-restored',received:Math.min(i,120)*60+Math.max(0,i-120)*tail,decoded:Math.min(i,120)*60+Math.max(0,i-120)*tail,composed:Math.min(i,120)*60+Math.max(0,i-120)*tail,frames:Array.from({length:i>120?tail:60}),camera:{decoded:i},flashes:i%2?[i*1000]:[],beeps:i%2?[i*1000+40]:[]}));}
assert(highWindows(fixture()).every(w=>w.passed));
assert(highWindows(fixture(10)).some(w=>!w.passed),'late high collapse must fail');
const wrongRtp=fixture();wrongRtp.forEach((s,i)=>{s.received=i*40;});assert(highWindows(wrongRtp).some(w=>!w.passed),'presentation cannot mask RTP loss');
const wrongPresent=fixture();wrongPresent.forEach((s,i)=>{s.composed=i*39;});assert(highWindows(wrongPresent).some(w=>!w.passed),'RTP cannot mask presentation loss');
assert.equal(rates(fixture())[0].received,60);
const composition=fixture();composition.forEach((s,i)=>{s.composed=i*60;s.frames.length=50;});
assert.equal(rates(composition)[0].composed,60);
assert.equal(rates(composition)[0].presented,50);
// rVFC callbacks are skipped while the probe keeps the main thread busy (spec);
// the compositor's presentedFrames is the acceptance count (07/10/2026: 57.7 composed, 49 callbacks, 0 dropped).
assert(highWindows(composition).every(w=>w.passed),'callback gaps alone must not fail a fully composed stream');
const noComposed=fixture();noComposed.forEach(s=>{s.composed=null;});assert(highWindows(noComposed).some(w=>!w.passed),'missing compositor counter must fail');
assert(cameraGrowing(fixture()));const frozen=fixture();frozen.forEach(s=>s.camera.decoded=1);assert(!cameraGrowing(frozen),'one camera frame is not activity');
assert(syncWindows(fixture()).every(w=>w.passed));const drift=fixture();drift.filter(s=>s.elapsed>120).forEach(s=>s.beeps=s.beeps.map(t=>t+160));assert(syncWindows(drift).some(w=>!w.passed),'late AV drift (200 ms) must fail');
const lateOk=fixture();lateOk.filter(s=>s.elapsed>120).forEach(s=>s.beeps=s.beeps.map(t=>t+130));assert(syncWindows(lateOk).every(w=>w.passed),'sound 170 ms late (40+130) is under the BT.1359 acceptability lag limit');
const early=fixture();early.filter(s=>s.elapsed>120).forEach(s=>s.beeps=s.beeps.map(t=>t-140));assert(syncWindows(early).some(w=>!w.passed),'sound 100 ms early (40-140) must fail (BT.1359 lead limit 90)');
const smallLead=fixture();smallLead.filter(s=>s.phase==='low').forEach(s=>s.beeps=s.beeps.map(t=>t-110));assert(syncWindows(smallLead).every(w=>w.passed),'a lead on the 15 fps layer (frame quantisation) is reported, not judged');
assert(syncWindows(smallLead).some(w=>w.small_layer.offsets.includes(-70)),'small-layer offsets are still reported');
const highLead=fixture();highLead.filter(s=>s.phase==='high-initial').forEach(s=>s.beeps=s.beeps.map(t=>t-150));assert(syncWindows(highLead).some(w=>!w.passed),'the same lead on a high layer must fail');
const gapLead=fixture();gapLead.filter(s=>s.phase==='high-initial').forEach(s=>{s.beeps=s.beeps.map(t=>t-160);s.flash_gaps=s.flashes.map(()=>33);});assert(syncWindows(gapLead).every(w=>w.passed),'a 120 ms lead inside a 33 ms probe gap may be an 87 ms lead');
const gapLate=fixture();gapLate.filter(s=>s.phase==='high-initial').forEach(s=>{s.beeps=s.beeps.map(t=>t+160);s.flash_gaps=s.flashes.map(()=>33);});assert(syncWindows(gapLate).some(w=>!w.passed),'a probe gap never excuses lateness');
const gapHuge=fixture();gapHuge.filter(s=>s.phase==='high-initial').forEach(s=>{s.beeps=s.beeps.map(t=>t-300);s.flash_gaps=s.flashes.map(()=>400);});assert(syncWindows(gapHuge).some(w=>!w.passed),'a probe that missed 400 ms measured nothing');
const missing=fixture();missing.filter(s=>s.elapsed>120).forEach(s=>s.flashes=[]);assert(syncWindows(missing).some(w=>!w.passed),'missing late sync probes must fail');
console.log('W4 metrics regressions passed');

