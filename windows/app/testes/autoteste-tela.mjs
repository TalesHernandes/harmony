import {verifyFixtureEmail} from './autoteste-email.mjs';
// Real WGC + Windows process audio + unchanged Go + web spectator.
import { monitorPlacements, assertMonitorRefresh, assertMonitorWindow, measureDisplayCadence } from './autoteste-monitor.mjs';
import { chromium } from 'playwright';
import { assertNoPreviousRun, killTree } from './autoteste-processes.mjs';
import { followTrackSurface } from './autoteste-surface.mjs';
import { spawn } from 'node:child_process';
import { readFileSync, writeFileSync, mkdirSync, existsSync } from 'node:fs';
import { resolve, relative, isAbsolute } from 'node:path';
import { pathToFileURL } from 'node:url';
import { rates, highWindows, cameraGrowing, syncWindows, AUDIO_EARLY_MS, AUDIO_LATE_MS } from './autoteste-metrics.mjs';
import { originSync } from './autoteste-origin-sync.mjs';
import { resumoNativo } from '../../../scripts/bancada/nativo.mjs';
const root=resolve('.'),work=resolve('windows/app/queixas/w4');mkdirSync(work,{recursive:true});
const args=process.argv.slice(2);const index=args.indexOf('--minutos');const seconds=index<0?60:Number(args[index+1])*60;
const fpsIndex=args.indexOf('--fps'),requestedFps=fpsIndex<0?60:Number(args[fpsIndex+1]);
if(![15,30,60].includes(requestedFps))throw Error('Frame rate must be 15, 30 or 60');
if(!Number.isFinite(seconds)||seconds<10||seconds>3600)throw Error('Duration must be 10..3600 seconds');
const output=resolve(work,'autoteste-tela.json'),nativeOutput=resolve(work,'autoteste-tela-nativo.json');
const withDeadline=(promise,step,ms)=>{let timer;return Promise.race([promise,new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('Deadline: '+step)),ms);})]).finally(()=>clearTimeout(timer));};
const pause=ms=>new Promise(r=>setTimeout(r,ms));
const result={requested_fps:requestedFps,capture_kind:args.includes('--monitor')?'monitor':'window',passed:false,mode:'w4',duration_seconds:seconds,stage:'starting',fps:null,stalls:null,av_sync_ms:null,call_leak:null,camera:'não encontrado',mobile:'não encontrado'};
let placements;
let diagnosticPage,browser,native,game,stop,phase='server',errorCode='não encontrado';
const save=()=>writeFileSync(output,JSON.stringify(result,null,2),'utf8');save();
async function waitFile(path,predicate,child,timeout=45000) {
  const end=Date.now()+timeout;
  while(Date.now()<end) {
    if(existsSync(path)) {try {const value=JSON.parse(readFileSync(path,'utf8'));if(predicate(value))return value;if(child?.exitCode!=null)throw Error(value.stage||'Child failed');}catch(e){if(e instanceof SyntaxError){}else throw e;}}
    if(child?.exitCode!=null)throw Error('Child exited');await pause(25);
  }
  throw Error('Child readiness timeout');
}
async function monitorCommand(command) {
  const path=resolve(work,'monitor-query.json');writeFileSync(path,'null');
  const child=spawn(resolve(work,'media-game.exe'),[path,...command],{stdio:'ignore',windowsHide:true,env:{...process.env,HARMONY_TEST_PARENT_PID:String(process.pid)}});
  try {
    await withDeadline(new Promise((resolve,reject)=>{child.once('error',reject);child.once('exit',code=>code===0?resolve():reject(Error('Monitor query failed: '+code)));}),'monitor-query',5000);
    return JSON.parse(readFileSync(path,'utf8'));
  } finally {if(child.exitCode==null)await killTree(child);}
}
async function placeViewer(page) {
  const title='Harmony-W4-viewer-'+process.pid;
  await page.evaluate(title=>{document.title=title;},title);
  const r=placements.viewer.rect;
  const actual=await monitorCommand(['--place-title',title,String(r.left),String(r.top),String(r.right-r.left),String(r.bottom-r.top)]);
  result.monitor_control.viewer={...result.monitor_control.viewer,...actual};
  (result.monitor_control.viewer_checks??=[]).push(actual);save();assertMonitorWindow(actual,placements.viewer);
  return actual;
}
try {
  phase='preflight';await assertNoPreviousRun(root);
  phase='monitor-preparation';result.failure_kind='preparation';
  if(process.env.W4_HEADED==='0')throw Error('Monitor control requires W4_HEADED=1');
  const monitors=await monitorCommand(['--monitors']);
  placements=monitorPlacements(monitors,process.env);
  const requested=Object.fromEntries(Object.entries(placements).map(([role,p])=>[role,{selector:p.selector,monitor:p.monitor,rect:p.rect}]));
  result.monitor_control={requested,available:monitors,headed:true};save();
  assertMonitorRefresh(placements,process.env);
  // Keep the existing disposable harness and every output/cache inside this tree.
  let source=readFileSync('e2e-harmony/server.mjs','utf8');
  const serverFile=resolve(work,'server.mjs');writeFileSync(serverFile,source,'utf8');
  const server=await import(pathToFileURL(serverFile));stop=server.stop;
  const {invites}=await server.start(3),base=server.BASE;
  const password=readFileSync('e2e-harmony/fixtures.ts','utf8').match(/export const PASSWORD = '([^']+)';/)[1]; // harmony:allow-secret (the local test fixture's password, read from e2e-harmony/fixtures.ts)
  const users=['w4-native','w4-viewer','w4-call'].map(n=>n+'-'+Date.now().toString(36));
  for(let i=0;i<3;i++) {
    const r=await fetch(base+'/api/signup',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({code:invites[i],username:users[i],display_name:['Transmissor','Espectador','Voz de teste'][i],password,email:users[i]+'@example.invalid'})});
    if(!r.ok)throw Error('Fixture signup failed');
    await verifyFixtureEmail(r,users[i]+'@example.invalid',base);
  }
  phase='game';const gameOutput=resolve(work,'janela-jogo.json');writeFileSync(gameOutput,'{}','utf8');
  game=spawn(resolve(work,'media-game.exe'),[gameOutput],{stdio:'ignore',windowsHide:true,env:{...process.env,HARMONY_TEST_PARENT_PID:String(process.pid),W4_WINDOW_RECT:[placements.game.rect.left,placements.game.rect.top,placements.game.rect.right-placements.game.rect.left,placements.game.rect.bottom-placements.game.rect.top].join(',')}});
  const window=await waitFile(gameOutput,v=>v.hwnd,game);
  result.monitor_control.game=window.monitor;save();assertMonitorWindow(window.monitor,placements.game);
  if(!window.audio_ready)throw Error(window.audio_failure||'CABLE Input unavailable');
  if(!window.foreground_available)throw Error('interactive_desktop_unavailable');
  phase='native';writeFileSync(nativeOutput,'{}','utf8');
  native=spawn(resolve(process.env.HARMONY_TEST_EXE||'windows/app/out/harmony-nativo.exe'),['--autoteste-tela',nativeOutput],{windowsHide:true,stdio:['ignore','ignore','pipe'],env:{...process.env,HARMONY_TEST_PARENT_PID:String(process.pid),HARMONY_URL:base,HARMONY_TEST_USER:users[0],HARMONY_TEST_VISITOR:users[2],HARMONY_TEST_PASSWORD:password,HARMONY_TEST_WINDOW:String(window.hwnd),HARMONY_TEST_SECONDS:String(seconds+25),HARMONY_TEST_MONITOR:args.includes('--monitor')?'1':'0',HARMONY_TEST_FPS:String(requestedFps),HARMONY_TEST_REQUIRE_CABLE:'1',HARMONY_TEST_CAMERA:args.includes('--camera')?'1':'0'}}); // harmony:allow-secret (the local test fixture's password, read from e2e-harmony/fixtures.ts)
  // Only the native sanitized diagnostic ring is permitted to produce log text.
  native.stderr.on('data',()=>{});
  await waitFile(nativeOutput,v=>v.stage==='transmitting',native);
  phase='browser';browser=await chromium.launch({headless:false,args:['--window-position='+placements.viewer.rect.left+','+placements.viewer.rect.top,'--mute-audio','--use-fake-ui-for-media-stream','--use-fake-device-for-media-stream','--allow-loopback-in-peer-connection','--autoplay-policy=no-user-gesture-required']});
  async function client(username,tone=false) {
    const context=await browser.newContext({viewport:{width:1600,height:900},permissions:['microphone']});
    if(!(await context.request.post(base+'/api/login',{data:{username,password}})).ok())throw Error('Fixture login failed');
    const page=await context.newPage();page.setDefaultTimeout(15000);page.setDefaultNavigationTimeout(15000);
    const evaluate=page.evaluate.bind(page);page.evaluate=(fn,arg)=>withDeadline(evaluate(fn,arg),phase+":page.evaluate",15000);
    await page.addInitScript(({tone,user,noPixels})=>{
      if(tone)localStorage.setItem(`harmony.voice.${user}.micfilters`,JSON.stringify({noise:false,echo:false,agc:false,rnnoise:false}));
      window.w4={pcs:[],tracks:{},session:'',frames:[],flashes:[],flashGaps:[],beeps:[],audio:[],lastFlash:false,layerRequests:[],refresh:[],noPixels};
      const capture=offer=>{if(offer?.session_id)window.w4.session=offer.session_id;for(const t of offer?.tracks||[])window.w4.tracks[t.mid]=t;};
      const originalFetch=window.fetch;window.fetch=async(...args)=>{if(String(args[0]).endsWith('/api/voice/layer')){try{const body=JSON.parse(args[1]?.body||'{}');window.w4.layerRequests.push({at:performance.now(),layer:body.layer});}catch{}}const response=await originalFetch(...args);if(String(args[0]).includes('/api/voice/'))response.clone().json().then(capture).catch(()=>{});return response;};
      const Socket=window.WebSocket;window.WebSocket=class extends Socket{constructor(...args){super(...args);this.addEventListener('message',e=>{try{const event=JSON.parse(e.data);if(event.type==='voice_offer')capture(event.data);}catch{}});}};
      const PC=window.RTCPeerConnection;window.RTCPeerConnection=class extends PC{constructor(...args){super(...args);window.w4.pcs.push(this);}};
      if(tone)navigator.mediaDevices.getUserMedia=async()=>{const ac=new AudioContext(),osc=ac.createOscillator(),gain=ac.createGain(),dest=ac.createMediaStreamDestination();osc.frequency.value=2300;gain.gain.value=0.2;osc.connect(gain).connect(dest);osc.start();let timer;try{await Promise.race([ac.resume(),new Promise((_,reject)=>{timer=setTimeout(()=>reject(Error('Deadline: tone-audio-resume')),3000);})]);}finally{clearTimeout(timer);}return dest.stream;};
    },{tone,user:username,noPixels:process.env.W4_NO_PIXELS==='1'});
    await page.goto(base);const later=page.getByRole('button',{name:'Agora não',exact:true}),join=page.getByTitle('Entrar na sala',{exact:true}).first();
    // The e-mail prompt renders after the first paint: wait for it or the room list, never count once.
    try{await later.or(join).first().waitFor({timeout:15000});if(await later.count())await later.click();}catch(error){await page.screenshot({path:resolve(work,`falha-email-${username}.png`)}).catch(()=>{});throw error;}
    try{await page.getByTitle('Entrar na sala',{exact:true}).first().click({timeout:15000});}catch(error){await page.screenshot({path:resolve(work,`falha-${username}.png`)}).catch(()=>{});writeFileSync(resolve(work,`falha-${username}.html`),await page.content(),'utf8');throw error;}return {page,context};
  }
  const viewer=await client(users[1]),call=await client(users[2],true);
  diagnosticPage=viewer.page;phase='watch';await viewer.page.bringToFront();
  await viewer.page.waitForFunction(()=>window.w4.session);
  await viewer.page.setViewportSize({width:1920,height:1200});
  await placeViewer(viewer.page);
  try{await viewer.page.getByRole('button',{name:'assistir à tela',exact:true}).first().click();}catch(error){await viewer.page.screenshot({path:resolve(work,'falha-assistir.png')}).catch(()=>{});throw error;}
  await viewer.page.waitForFunction(()=>window.w4.pcs.some(pc=>pc.getTransceivers().some(tr=>window.w4.tracks[tr.mid]?.source==='screen'&&tr.receiver.track.readyState==='live')));
  await viewer.page.waitForFunction(()=>{const state=window.w4,track=state.pcs.flatMap(pc=>pc.getTransceivers()).find(tr=>state.tracks[tr.mid]?.source==='screen')?.receiver.track;return [...document.querySelectorAll('video')].some(v=>v.srcObject?.getVideoTracks().some(t=>t.id===track?.id));});
  phase='receiver-probes';result.stage=phase;save();
  await viewer.page.evaluate('window.followTrackSurface=('+followTrackSurface.toString()+')');
  await viewer.page.evaluate(async()=>{
    const state=window.w4;
    function refresh(now){state.refresh.push(now);requestAnimationFrame(refresh);}requestAnimationFrame(refresh);
    const transceivers=state.pcs.flatMap(pc=>pc.getTransceivers());
    const videoTrack=transceivers.find(tr=>state.tracks[tr.mid]?.source==='screen').receiver.track;
    const audioTrack=transceivers.find(tr=>state.tracks[tr.mid]?.source==='screen_audio')?.receiver.track;
    if(!audioTrack)throw Error('Game audio track missing');
    // Measure the real web surface: its ResizeObserver owns the layer request.
    let video=[...document.querySelectorAll('video')].find(v=>v.srcObject?.getVideoTracks().some(t=>t.id===videoTrack.id));
    if(!video)throw Error('Web screen surface missing');
    const followTrackSurface=window.followTrackSurface;
    state.surface=followTrackSurface({state,track:videoTrack,find:()=>[...document.querySelectorAll('video')],onFrame:(surface,now,metadata)=>{video=surface;return frame(now,metadata);}});
    if(typeof VideoFrame==='undefined')throw Error('VideoFrame pixel probe unavailable');
    const ac=new AudioContext({sampleRate:48000}),src=ac.createMediaStreamSource(new MediaStream([audioTrack])),analyser=ac.createAnalyser();state.audioContext=ac;analyser.fftSize=2048;src.connect(analyser);await state.surface.deadline(ac.resume(),'audio-resume');
    const pcm=new Float32Array(analyser.fftSize),spectrum=new Float32Array(analyser.frequencyBinCount);let audioOn=false;
    setInterval(()=>{analyser.getFloatTimeDomainData(pcm);const rms=Math.sqrt(pcm.reduce((s,x)=>s+x*x,0)/pcm.length);const on=rms>0.015;if(on&&!audioOn)state.beeps.push(performance.now());audioOn=on;analyser.getFloatFrequencyData(spectrum);const bin=hz=>Math.round(hz/ac.sampleRate*analyser.fftSize);state.audio.push({at:performance.now(),context_time:ac.currentTime,rms,game_db:spectrum[bin(1000)],call_db:Math.max(...spectrum.slice(bin(2300)-2,bin(2300)+3))});},10);
    async function frame(now,metadata){state.surface.next();state.frames.push({at:now,media:metadata.mediaTime,width:metadata.width,height:metadata.height,presentedFrames:metadata.presentedFrames,presentationTime:metadata.presentationTime,expectedDisplayTime:metadata.expectedDisplayTime,rtpTimestamp:metadata.rtpTimestamp,captureTime:metadata.captureTime,receiveTime:metadata.receiveTime,processingDuration:metadata.processingDuration});state.composed=state.surface.composed(metadata.presentedFrames);if(state.pixelBusy||state.noPixels)return;state.pixelBusy=true;const prevProbe=state.lastProbe;state.lastProbe={at:now,presented:metadata.presentedFrames};let sample;try{sample=new VideoFrame(video);const bytes=new Uint8Array(16);await state.surface.deadline(sample.copyTo(bytes,{format:'RGBA',rect:{x:Math.floor(sample.displayWidth/2),y:Math.floor(sample.displayHeight*0.065),width:2,height:2}}),'pixel-center');const markerHeight=Math.max(2,Math.floor(sample.displayHeight*0.10));const marker=new Uint8Array(8*markerHeight);await state.surface.deadline(sample.copyTo(marker,{format:'RGBA',rect:{x:Math.floor(sample.displayWidth/2)-Math.floor(sample.displayWidth*0.06),y:Math.floor(sample.displayHeight*0.16),width:2,height:markerHeight}}),'pixel-notice');state.noticeSamples=(state.noticeSamples||0)+1;for(let i=0;i<marker.length;i+=4)if(marker[i]>200&&marker[i+1]<80&&marker[i+2]>200)state.noticeLeaked=true;const white=bytes[0]>220&&bytes[1]>220&&bytes[2]>220;if(white&&!state.lastFlash){state.flashes.push(now);state.flashGaps.push(prevProbe&&metadata.presentedFrames-prevProbe.presented>1?now-prevProbe.at:0);}state.lastFlash=white;}catch{state.pixel_probe_failed=true;}finally{sample?.close();state.pixelBusy=false;}}await state.surface.ready();
  });
  await pause(5000);
  phase='monitor-preparation';
  await placeViewer(viewer.page);
  Object.assign(result.monitor_control.viewer,await viewer.page.evaluate(measureDisplayCadence));
  result.monitor_control.game=await monitorCommand(['--inspect-window',String(window.hwnd)]);
  Object.assign(result.monitor_control.viewer,await monitorCommand(['--inspect-window',String(result.monitor_control.viewer.hwnd)]));
  save();assertMonitorWindow(result.monitor_control.game,placements.game);assertMonitorWindow(result.monitor_control.viewer,placements.viewer);
  result.monitor_control.browser_version=browser.version();
  result.monitor_control.prepared=true;delete result.failure_kind;
  phase='measuring';result.stage=phase;save();
  const started=Date.now(),samples=[];let shrunk=false,restored=false;
  while(Date.now()-started<seconds*1000) {
    await pause(1000);
    const elapsed=(Date.now()-started)/1000;
    if(seconds<1800&&!shrunk&&elapsed>=seconds/3){await viewer.page.setViewportSize({width:400,height:300});await placeViewer(viewer.page);shrunk=true;}
    if(seconds<1800&&!restored&&elapsed>=seconds*2/3){await viewer.page.setViewportSize({width:1920,height:1200});await placeViewer(viewer.page);restored=true;}
    const sample=await viewer.page.evaluate(async()=>{
      const state=window.w4;const pc=state.pcs.find(pc=>pc.getTransceivers().some(tr=>state.tracks[tr.mid]?.source==='screen'));
      const mid=pc.getTransceivers().find(tr=>state.tracks[tr.mid]?.source==='screen').mid;
      if(state.surfaceFailure)throw Error(state.surfaceFailure);
      const report=[...(await state.surface.deadline(pc.getStats(),'rtp-stats')).values()];const stats=report.find(s=>s.type==='inbound-rtp'&&s.mid===mid&&s.kind==='video');const cameraMid=pc.getTransceivers().find(tr=>state.tracks[tr.mid]?.source==='cam')?.mid;const camera=report.find(s=>s.type==='inbound-rtp'&&s.mid===cameraMid&&s.kind==='video');const audioMid=pc.getTransceivers().find(tr=>state.tracks[tr.mid]?.source==='screen_audio')?.mid;const sound=report.find(s=>s.type==='inbound-rtp'&&s.mid===audioMid&&s.kind==='audio');
      const quality=state.surface.quality();
      return {time_origin_ms:performance.timeOrigin,composed:state.composed??null,visibility:document.visibilityState,refresh:state.refresh.splice(0),layer_requests:state.layerRequests.splice(0),playback_quality:{total:quality.totalVideoFrames,dropped:quality.droppedVideoFrames},audio_probe:{source:'receiver MediaStream; separate AudioContext; before device output',state:state.audioContext.state,current_time:state.audioContext.currentTime,base_latency:state.audioContext.baseLatency??null,output_latency:state.audioContext.outputLatency??null,output_timestamp:state.audioContext.getOutputTimestamp?.()??null},audio_rtp_diagnostics:Object.fromEntries(['concealedSamples','silentConcealedSamples','concealmentEvents','insertedSamplesForDeceleration','removedSamplesForAcceleration','totalSamplesReceived','jitter','packetsLost'].map(k=>[k,sound?.[k]??null])),sync_stats:{video_at:stats?.timestamp??null,video_playout_ntp:stats?.estimatedPlayoutTimestamp??null,video_target:stats?.jitterBufferTargetDelay??null,video_minimum:stats?.jitterBufferMinimumDelay??null,audio_at:sound?.timestamp??null,audio_playout_ntp:sound?.estimatedPlayoutTimestamp??null,audio_delay:sound?.jitterBufferDelay??null,audio_emitted:sound?.jitterBufferEmittedCount??null,audio_target:sound?.jitterBufferTargetDelay??null,audio_minimum:sound?.jitterBufferMinimumDelay??null},rtp_diagnostics:Object.fromEntries(['packetsReceived','packetsLost','jitter','framesDropped','keyFramesDecoded','nackCount','pliCount','firCount','jitterBufferDelay','jitterBufferEmittedCount','totalDecodeTime','freezeCount','totalFreezesDuration'].map(k=>[k,stats?.[k]??null])),notice_samples:state.noticeSamples||0,notice_leaked:state.noticeLeaked||false,drawn_width:state.video.getBoundingClientRect().width*devicePixelRatio,camera:camera?{decoded:camera.framesDecoded,width:camera.frameWidth,height:camera.frameHeight}:null,at:performance.now(),decoded:stats?.framesDecoded??null,received:stats?.framesReceived??null,width:stats?.frameWidth??null,height:stats?.frameHeight??null,frames:state.frames.splice(0),flashes:state.flashes.splice(0),flash_gaps:state.flashGaps.splice(0),beeps:state.beeps.splice(0),audio:state.audio.splice(0),pixel_probe_failed:state.pixel_probe_failed||false};
    });sample.elapsed=elapsed;sample.phase=seconds>=1800?'high-live':!shrunk?'high-initial':!restored?'low':elapsed<seconds*2/3+5?'restoring':'high-restored';samples.push(sample);
    result.samples=samples;result.stage='measuring';save();
  }
  phase='summary';
  const median=values=>values.length?[...values].sort((a,b)=>a-b)[Math.floor(values.length/2)]:null;
  const highRates=samples.slice(1).flatMap((s,i)=>s.elapsed>5&&s.elapsed<seconds/3-2?[s.frames.length*1000/(s.at-samples[i].at)]:[]);
  const frames=samples.flatMap(s=>s.frames);const stalls=frames.slice(1).map((f,i)=>f.at-frames[i].at).filter(ms=>ms>100);
  const flashes=samples.flatMap(s=>s.flashes),beeps=samples.flatMap(s=>s.beeps);
  const offsets=flashes.map(f=>beeps.map(b=>b-f).filter(ms=>Math.abs(ms)<500).sort((a,b)=>Math.abs(a)-Math.abs(b))[0]).filter(Number.isFinite);
  const audio=samples.flatMap(s=>s.audio);const leakage=audio.filter(a=>a.game_db>-65&&Number.isFinite(a.call_db)).map(a=>a.call_db-a.game_db);
  result.rtp_rates=rates(samples);result.high_windows=highWindows(samples);if(requestedFps!==60)for(const w of result.high_windows)w.passed=w.received>=requestedFps*.95&&w.decoded>=requestedFps*.95&&w.composed>=requestedFps*.95;result.sync_windows=syncWindows(samples);
  result.fps=median(result.high_windows.map(w=>w.received));result.stalls={count:stalls.length,max_ms:stalls.length?Math.max(...stalls):0};result.pixel_probe=process.env.W4_NO_PIXELS==='1'?'off: diagnostic only, sync and notice not measured':'on';result.av_sync_ms={median:median(offsets),max_absolute:offsets.length?Math.max(...offsets.map(Math.abs)):null,pairs:offsets.length,measurement_uncertainty_ms:'não encontrado',audio_poll_resolution_bound_ms:53,web_target:'não encontrado',itu_bt1359_ms:{early:AUDIO_EARLY_MS,late:AUDIO_LATE_MS}};
  result.call_leak={relative_db:median(leakage),threshold_db:-30};
  result.layer_switch=seconds>=1800||samples.some(s=>s.elapsed>seconds/3+5&&s.elapsed<seconds*2/3&&s.width<=480)&&samples.some(s=>s.elapsed>seconds*2/3+5&&s.width>=1200);
  result.notice_exclusion={pixel_samples:samples.at(-1)?.notice_samples??0,leaked:samples.some(s=>s.notice_leaked)};
  result.passed=result.notice_exclusion.pixel_samples>seconds*5&&!result.notice_exclusion.leaked&&result.high_windows.length>0&&result.high_windows.every(w=>w.passed)&&result.sync_windows.length>0&&result.sync_windows.every(w=>w.passed)&&result.layer_switch&&offsets.length>=Math.floor(seconds/3)&&!samples.some(s=>s.pixel_probe_failed)&&result.call_leak.relative_db!==null&&result.call_leak.relative_db<-30;
  // This is the stated browser proof only. Camera/mobile and capture exclusion
  // remain separately visible as pending; a browser PASS does not accept W4.
  if(args.includes('--camera')) {const seen=samples.filter(s=>s.camera?.decoded>0);result.camera={received:cameraGrowing(samples),frames:seen.at(-1)?.camera.decoded??null,width:seen.at(-1)?.camera.width??null,height:seen.at(-1)?.camera.height??null};result.passed=result.passed&&result.camera.received;}
  result.stage='browser_done';result.game=await waitFile(gameOutput,v=>v.cadence_samples?.length>0,game,3000);save();
  const nativeResult=await waitFile(nativeOutput,v=>v.stage==='done',native,35000);result.native=resumoNativo(nativeResult.native_measurements||[]);result.native_samples=nativeResult.native_measurements;result.layer_mapping=nativeResult.layer_mapping;result.screen_diagnostics=nativeResult.screen_diagnostics;result.notice_exclusion.native_visible=nativeResult.notice_visible===true;result.notice_exclusion.native_affinity_confirmed=nativeResult.notice_excluded===true;result.passed=result.passed&&result.notice_exclusion.native_visible&&result.notice_exclusion.native_affinity_confirmed;result.native_call_playout_confirmed=result.native_samples.filter(s=>s.call_playout_confirmed).length>=5;result.passed=result.passed&&result.native_call_playout_confirmed;save();
  if(process.env.HARMONY_SYNC_PROBE==='1') {
    result.game=await waitFile(gameOutput,v=>v.cadence_samples?.length>0,game,3000);
    result.origin_sync=originSync(result.native_samples,result.game,samples,nativeResult.sync_probe_tail);save();
  }
  const md=`# Bancada W4\n\nResultado do navegador: ${result.passed?'passou':'falhou'}. Duração: ${seconds} s.\n\nQuadros na camada alta: ${result.fps??'não encontrado'}. Travadas: ${result.stalls.count}. Atraso som–imagem mediano: ${result.av_sync_ms.median??'não encontrado'} ms (incerteza: não encontrado; limite de resolução temporal de áudio: 53 ms; meta web: não encontrado).\n\nCâmera e celular: não encontrado. Este resultado não conclui o aceite do sprint.\n`;
  writeFileSync(resolve(work,'bancada-w4.md'),md,'utf8');
  if(!result.passed)process.exitCode=1;
} catch(error) {
  if(diagnosticPage)await withDeadline(diagnosticPage.screenshot({path:resolve(work,`falha-${phase}.png`)}),'failure-screenshot',5000).catch(()=>{});
  result.failure_kind='preparation';
  errorCode=error.code||error.cause?.code||'não encontrado';result.stage=phase;result.failure_code=errorCode;result.failure_step=phase;result.failure_detail=error.message;result.failure_reason=error.message==='interactive_desktop_unavailable'?'desktop interativo não disponível':error.message.includes('CABLE Input')?error.message:'não encontrado';save();process.exitCode=1;
  console.error('W4 falhou em '+phase+'; código '+errorCode+' '+(error?.stack||error));
} finally {for(const child of [native,game])try{await killTree(child);}catch(error){result.cleanup_failure=error.message;result.passed=false;process.exitCode=1;save();}try{await withDeadline(browser?.close(),'browser-close',10000);}catch{}stop?.();}
console.log('Resultado: windows/app/queixas/w4/autoteste-tela.json');
