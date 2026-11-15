// Diagnostic matching only. Never changes the spectator acceptance criteria.
const finite=Number.isFinite;
const missing='não encontrado';
const stamp=e=>e?.kind==='flash'?(finite(e.capture_us)?e.capture_us/1000:null):
  finite(e?.captured_ms)&&finite(e?.onset_ms)?e.captured_ms+e.onset_ms:null;
// A bounded, unique match in both directions. No reuse, interpolation or cycle filling.
export function uniquePairs(left,right,timeLeft,timeRight,limit=750) {
  const candidates=left.map(a=>right.map((b,i)=>({i,d:finite(timeRight(b))&&finite(timeLeft(a))?timeRight(b)-timeLeft(a):NaN}))
    .filter(x=>finite(x.d)&&Math.abs(x.d)<limit));
  return candidates.map(c=>c.length===1&&candidates.filter(other=>other.some(x=>x.i===c[0].i)).length===1?right[c[0].i]:null);
}
function distribution(values) {
  const v=values.filter(finite).sort((a,b)=>a-b),n=v.length;
  return n?{count:n,min:v[0],median:(v[Math.floor((n-1)/2)]+v[Math.ceil((n-1)/2)])/2,max:v[n-1]}:
    {count:0,min:missing,median:missing,max:missing};
}
export function originSync(nativeSamples=[],game={},samples=[],tail={}) {
  const batches=[...nativeSamples.map(s=>s.sync_probe).filter(Boolean),tail];
  const events=batches.flatMap(b=>b.events??[]);
  const gameEvents=game.sync_events??[];
  const groups=new Map();
  for(const e of gameEvents) {
    if(!finite(e.event_id)||!['flash','beep'].includes(e.kind))continue;
    if(!groups.has(e.event_id))groups.set(e.event_id,{event_id:e.event_id,flash:[],beep:[]});
    groups.get(e.event_id)[e.kind].push(e);
  }
  const rows=[...groups.values()].sort((a,b)=>a.event_id-b.event_id);
  for(const kind of ['flash','beep']) {
    const g=rows.map(r=>r[kind].length===1?r[kind][0]:null);
    const good=g.filter(Boolean),n=events.filter(e=>e.kind===kind && (kind!=='beep' || e.source==='screen_audio'));
    // Unlabelled R25 audio also contains the AEC reference. It cannot identify the published track.
    const matches=uniquePairs(good,n,e=>e.wall_ms,e=>e.wall_ms);
    for(let i=0;i<good.length;++i) {
      const row=rows.find(r=>r.event_id===good[i].event_id);
      row['game_'+kind]=good[i];row['origin_'+kind]=matches[i];
    }
  }
  const flashes=samples.flatMap(s=>(s.flashes??[]).map((at,i)=>({at,wall_ms:finite(s.time_origin_ms)?s.time_origin_ms+at:null,phase:s.phase,gap_ms:s.flash_gaps?.[i]??null})));
  const beeps=samples.flatMap(s=>(s.beeps??[]).map(at=>({wall_ms:finite(s.time_origin_ms)?s.time_origin_ms+at:null})));
  const viewerBeeps=uniquePairs(flashes,beeps,e=>e.wall_ms,e=>e.wall_ms,500);
  const gameFlashes=rows.filter(r=>r.game_flash);
  const viewerFlashes=uniquePairs(gameFlashes,flashes,r=>r.game_flash.wall_ms,f=>finite(f.wall_ms)?f.wall_ms:NaN,900);
  for(const row of rows) {
    delete row.flash;delete row.beep;
    const f=row.origin_flash,b=row.origin_beep,fs=stamp(f),bs=stamp(b);
    row.origem_offset_ms=finite(fs)&&finite(bs)?bs-fs:missing;
    const gf=row.game_flash,gb=row.game_beep;
    row.jogo_offset_ms=finite(gf?.qpc_ms)&&finite(gb?.qpc_ms)&&gb.submitted!==false?gb.qpc_ms-gf.qpc_ms:missing;
    row.jogo_scheduled_offset_ms=finite(gf?.tick_ms)&&finite(gb?.scheduled_tick_ms)?gb.scheduled_tick_ms-gf.tick_ms:missing;
    for(const [kind,e,g] of [['flash',f,gf],['beep',b,gb]]) {
      const s=stamp(e);
      // Wall/monotonic anchors are sampled together at observation, not inferred from perf.s.
      row[kind+'_capture_wall_ms']=e&&finite(s)&&finite(e.media_now_ms)?e.wall_ms+s-e.media_now_ms:missing;
      row[kind+'_capture_minus_game_ms']=finite(row[kind+'_capture_wall_ms'])&&g?row[kind+'_capture_wall_ms']-g.wall_ms:missing;
      row[kind+'_observation_minus_capture_ms']=e&&finite(s)&&finite(e.media_now_ms)?e.media_now_ms-s:missing;
    }
    const vf=viewerFlashes[gameFlashes.indexOf(row)];
    const vb=vf?viewerBeeps[flashes.indexOf(vf)]:null;
    row.phase=vf?.phase??missing;
    row.viewer_flash=vf??missing;
    row.viewer_offset_ms=vf&&finite(vb?.wall_ms)?vb.wall_ms-vf.wall_ms:missing;
    row.added_offset_ms=finite(row.viewer_offset_ms)&&finite(row.origem_offset_ms)?row.viewer_offset_ms-row.origem_offset_ms:missing;
    row.status=finite(row.origem_offset_ms)?'paired':missing;
  }
  const phases=Object.fromEntries([...new Set(rows.map(r=>r.phase))].map(phase=>{
    const p=rows.filter(r=>r.phase===phase);
    return [phase,Object.fromEntries(['origem_offset_ms','jogo_offset_ms','viewer_offset_ms','added_offset_ms','flash_capture_minus_game_ms','beep_capture_minus_game_ms'].map(k=>[k,distribution(p.map(r=>r[k]))]))];
  }));
  return {unattributed_audio_events:events.filter(e=>e.kind==='beep'&&!e.source).length,enabled:batches.some(b=>b.enabled),matching:'unique both ways; origin observation/game <750ms; viewer/game <900ms; viewer beep/flash <500ms',
    lost:batches.reduce((sum,b)=>sum+(b.lost??0),0)+(game.sync_events_lost??0),
    gaps:events.filter(e=>e.kind.endsWith('_gap')),native_event_count:events.length,game_event_count:gameEvents.length,
    rows,phases};
}
