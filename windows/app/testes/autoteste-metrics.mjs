// RTP rates and presentation counts deliberately stay separate.
export function rates(samples) {
  return samples.slice(1).map((s,i)=>{
    const p=samples[i], seconds=(s.at-p.at)/1000;
    const delta=key=>Number.isFinite(s[key])&&Number.isFinite(p[key])&&s[key]>=p[key]?(s[key]-p[key])/seconds:null;
    return {elapsed:s.elapsed,seconds,phase:s.phase,received:delta('received'),decoded:delta('decoded'),composed:delta('composed'),presented:(s.frames?.length??0)/seconds};
  });
}
export function highWindows(samples) {
  const output=[];let group=[],phase;
  const flush=()=>{if(!group.length)return;const duration=group.reduce((n,s)=>n+s.seconds,0);const average=key=>group.every(s=>s[key]!==null)?group.reduce((n,s)=>n+s[key]*s.seconds,0)/duration:null;output.push({phase,start_seconds:group[0].elapsed-group[0].seconds,end_seconds:group.at(-1).elapsed,duration_seconds:duration,received:average('received'),decoded:average('decoded'),presented:average('presented'),composed:average('composed'),passed:average('received')>=57&&average('decoded')>=57&&average('composed')>=57});group=[];};
  for(const s of rates(samples)) {
    if(s.phase!==phase){flush();phase=s.phase;}
    if(!phase?.startsWith('high'))continue;
    group.push(s);if(group.reduce((n,s)=>n+s.seconds,0)>=60)flush();
  }
  flush();return output;
}
export function cameraGrowing(samples) {
  const recent=samples.slice(1).map((s,i)=>s.camera&&samples[i].camera&&s.camera.decoded>samples[i].camera.decoded);
  return recent.length>=5&&recent.every(Boolean);
}
// Lip-sync limits from ITU-R BT.1359 (acceptability): sound may lead the
// picture by up to 90 ms and lag it by up to 185 ms before it bothers people.
// offset = beep minus flash, so positive means the sound arrived late.
// The owner chose acceptability over detectability (45/125) on 11/10/2026:
// the stricter pair failed on start-up settling that live streams commonly show.
export const AUDIO_EARLY_MS=90,AUDIO_LATE_MS=185;
// A probe that skipped more than ~3 frames before a flash did not measure it.
export const MAX_GAP_MS=50;
// Judged only while the viewer is on a high layer: that is the full-screen
// picture the acceptance is about. On the 15 fps layer a flash can only show
// on a 66 ms frame boundary, so the sound "leads" by up to 66 ms by
// construction (r24: every lead past 45 ms fell in that phase). Those pairs
// are reported under small_layer, not judged (08/10/2026).
export function syncWindows(samples) {
  const pairs=[];const beeps=samples.flatMap(s=>s.beeps);
  // gap: when the pixel probe skipped presented frames before seeing a flash,
  // the flash really appeared somewhere in that gap, so the sound's lead is
  // only known to within it. Lateness is judged at the measured value (the
  // flash cannot have appeared after it was seen); the lead limit applies to
  // the earliest the flash could have appeared.
  for(const s of samples)s.flashes.forEach((f,j)=>{const gap=s.flash_gaps?.[j]??0;const offset=beeps.map(b=>b-f).filter(ms=>Math.abs(ms)<500).sort((a,b)=>Math.abs(a)-Math.abs(b))[0];pairs.push({elapsed:s.elapsed,high:!!s.phase?.startsWith('high'),gap,offset:Number.isFinite(offset)?offset:null});});
  const highSeconds=(start,end)=>samples.slice(1).reduce((n,s,i)=>s.elapsed>start&&s.elapsed<=end&&s.phase?.startsWith('high')?n+(s.elapsed-samples[i].elapsed):n,0);
  const windows=[];
  for(let start=0;start<(samples.at(-1)?.elapsed??0);start+=60){
    const end=Math.min(start+60,samples.at(-1).elapsed),all=pairs.filter(p=>p.elapsed>start&&p.elapsed<=end),group=all.filter(p=>p.high&&p.gap<=MAX_GAP_MS),small=all.filter(p=>!p.high);
    windows.push({start,end,pairs:group.length,max_absolute:group.every(p=>p.offset!==null)?Math.max(0,...group.map(p=>Math.abs(p.offset))):null,
      small_layer:{pairs:small.length,offsets:small.map(p=>p.offset===null?null:Math.round(p.offset))},
      uncertain_pairs:group.filter(p=>p.gap>0).length,unmeasured_pairs:all.filter(p=>p.high&&p.gap>MAX_GAP_MS).length,max_gap_ms:Math.round(Math.max(0,...group.map(p=>p.gap))),
      passed:group.length>=Math.floor(highSeconds(start,end)/3)&&group.every(p=>p.offset!==null&&p.offset+p.gap>=-AUDIO_EARLY_MS&&p.offset<=AUDIO_LATE_MS)});
  }
  return windows;
}
