// Coordinates are physical desktop pixels; numeric selectors are zero-based
// in device-name order, independent of EnumDisplayMonitors enumeration order.
export function monitorPlacement(monitors, selector='primary') {
  const list=[...monitors].sort((a,b)=>a.device.localeCompare(b.device,'en',{numeric:true}));
  const selected=selector==='primary'?list.find(m=>m.primary):selector==='secondary'?
    (list.filter(m=>!m.primary).length===1?list.find(m=>!m.primary):null):
    /^(0|[1-9][0-9]*)$/.test(selector)?list[Number(selector)]:null;
  if(!selected)throw Error('Requested monitor not found or ambiguous: '+selector);
  const r=selected.rect;
  if(!r||![r.left,r.top,r.right,r.bottom].every(Number.isInteger)||r.right<=r.left||r.bottom<=r.top)
    throw Error('Invalid monitor rectangle');
  const width=Math.min(1920,r.right-r.left),height=Math.min(1080,r.bottom-r.top);
  return {selector,monitor:selected,rect:{left:r.left,top:r.top,right:r.left+width,bottom:r.top+height}};
}
export function assertMonitorWindow(actual, placement) {
  const r=actual?.window_rect,m=placement.monitor.rect;
  if(actual?.device!==placement.monitor.device||!r||
    ![r.left,r.top,r.right,r.bottom].every(Number.isInteger)||r.right<=r.left||r.bottom<=r.top||
    r.left<m.left||r.top<m.top||r.right>m.right||r.bottom>m.bottom)
    throw Error('Window is not entirely inside requested monitor: '+placement.monitor.device);
}
// Runs in the page, before the measured run, with a bounded caller deadline.
export async function measureDisplayCadence() {
  const intervals=[];let first,last;
  await new Promise(resolve=>requestAnimationFrame(function tick(now){
    if(first===undefined)first=now;
    if(last!==undefined)intervals.push(now-last);last=now;
    if(now-first>=2000)resolve();else requestAnimationFrame(tick);
  }));
  const sorted=[...intervals].sort((a,b)=>a-b);
  const percentile=p=>{const i=(sorted.length-1)*p,l=Math.floor(i);return sorted[l]+(sorted[Math.ceil(i)]-sorted[l])*(i-l);};
  return {screen:{width:screen.width,height:screen.height,availLeft:screen.availLeft??'não encontrado',availTop:screen.availTop??'não encontrado'},
    devicePixelRatio,raf:{duration_ms:last-first,samples:intervals.length,median_ms:percentile(.5),p95_ms:percentile(.95)}};
}

// Role-specific settings override the shared setting; empty values keep legacy defaults.
export function monitorPlacements(monitors, env={}) {
  const shared=env.W4_MONITOR||'primary';
  return {game:monitorPlacement(monitors,env.W4_GAME_MONITOR||shared),
    viewer:monitorPlacement(monitors,env.W4_VIEWER_MONITOR||shared)};
}
export function assertMonitorRefresh(placements, env={}) {
  for(const role of ['game','viewer']) {
    const expected=env['W4_'+role.toUpperCase()+'_EXPECT_REFRESH_HZ']||env.W4_EXPECT_REFRESH_HZ;
    if(expected&&placements[role].monitor.refresh_hz!==Number(expected))
      throw Error('Requested '+role+' refresh rate does not match EnumDisplaySettings');
  }
}
