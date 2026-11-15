// Serialized into the page; all browser dependencies are supplied or global.
export function followTrackSurface({state,track,find,onFrame,timeout=3000,poll=50}) {
  let running=true,reload=false,video,stream,generation=0,pending=false,last=0,base=0,total=0,dropped=0,qualityBase=0,dropBase=0;
  let lastSeen=Date.now(),firstResolve,firstReject;
  const first=new Promise((resolve,reject)=>{firstResolve=resolve;firstReject=reject;});
  first.catch(()=>{});
  const fail=step=>{state.surfaceFailure='Deadline: receiver-probes:'+step;firstReject(Error(state.surfaceFailure));};
  const deadline=(promise,step)=>{let timer;return Promise.race([promise,new Promise((_,reject)=>{timer=setTimeout(()=>{fail(step);reject(Error(state.surfaceFailure));},timeout);})]).finally(()=>clearTimeout(timer));};
  function composed(n){if(!Number.isFinite(n))return null;if(n<last)base+=last;last=n;return base+n;}
  function quality(){const q=video?.getVideoPlaybackQuality();if(q){total=Math.max(total,qualityBase+q.totalVideoFrames);dropped=Math.max(dropped,dropBase+q.droppedVideoFrames);}return {totalVideoFrames:total,droppedVideoFrames:dropped};}
  function next(){if(!video||pending)return;pending=true;const current=video,token=generation;current.requestVideoFrameCallback((now,metadata)=>{if(token!==generation)return;pending=false;lastSeen=Date.now();firstResolve();onFrame(current,now,metadata);});}
  function loaded(){reload=true;refresh();}
  function refresh(){
    if(!running)return;
    const current=find().find(v=>v.srcObject?.getVideoTracks().some(t=>t.id===track.id));
    if(current&&(current!==video||current.srcObject!==stream||reload)){
      if(current!==video){video?.removeEventListener?.('emptied',loaded);video?.removeEventListener?.('loadedmetadata',loaded);current.addEventListener?.('emptied',loaded);current.addEventListener?.('loadedmetadata',loaded);}
      reload=false;quality();qualityBase=total;dropBase=dropped;base+=last;last=0;
      video=current;stream=current.srcObject;state.video=current;++generation;pending=false;
      const token=generation,playStarted=Date.now();
      // Never await play: the first delivered frame proves readiness. A stale or
      // aborted play request must not prevent following the replacement track.
      Promise.resolve().then(()=>current.play()).catch(error=>{if(token!==generation||lastSeen>playStarted)return;if(error.name==='AbortError')setTimeout(()=>{if(token===generation){reload=true;refresh();}},poll);else fail('surface-play:'+error.name);});
      next();
    }
    if(Date.now()-lastSeen>timeout)fail(current?'surface-frame':'surface-missing');
  }
  const timer=setInterval(refresh,poll);refresh();
  return {next,composed,quality,deadline,ready:()=>deadline(first,'surface-first-frame'),stop(){running=false;clearInterval(timer);video?.removeEventListener?.('emptied',loaded);video?.removeEventListener?.('loadedmetadata',loaded);++generation;}};
}
