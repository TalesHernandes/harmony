from pathlib import Path
import subprocess,sys,json
app=Path(__file__).resolve().parent.parent
out=app/'test-output'/'w4-regressions';out.mkdir(parents=True,exist_ok=True)
results=[]
def cpp(name,header=None,camera_header=None):
 directory=out/name;directory.mkdir(exist_ok=True)
 source=(app/'testes/w4-cadence-tests.cpp').read_text(encoding='utf-8')
 source=source.replace('#include "w4-timestamp-tests.h"', (app/'testes/w4-timestamp-tests.h').read_text(encoding='utf-8'))
 source=source.replace('../../native/src/camera-policy.h',str(camera_header or app.parent/'native/src/camera-policy.h').replace('\\','/'))
 source=source.replace('../../native/gpu/cadence.h',str(header or app.parent/'native/gpu/cadence.h').replace('\\','/'))
 translation=directory/'test.cpp';translation.write_text(source,encoding='utf-8');exe=directory/'test.exe'
 command=['cmd','/c',str(app.parent/'native/vc.cmd'),'cl','/nologo','/std:c++20','/EHsc','/MT','/utf-8',str(translation),'/Fo'+str(directory/'test.obj'),'/Fe'+str(exe)]
 built=subprocess.run(command,capture_output=True,text=True)
 (directory/'build.log').write_text(built.stdout+built.stderr,encoding='utf-8')
 if built.returncode:raise RuntimeError('build failed '+name)
 run=subprocess.run([str(exe)],capture_output=True,text=True);(directory/'test.log').write_text(run.stdout+run.stderr,encoding='utf-8');return run.returncode
if cpp('baseline'):raise RuntimeError('W4 cadence baseline failed')
subprocess.run(['node',str(app/'testes/w4-metrics-tests.mjs')],check=True)
subprocess.run(['node',str(app/'testes/w4-probe-tests.mjs')],check=True)
if '--mutate' in sys.argv:
 text=(app/'testes/autoteste-tela.mjs').read_text(encoding='utf-8')
 for name,before,after in [('blocking-pixel-probe','state.surface.next();state.frames.push','state.frames.push'),('notice-pixel-leak','state.noticeLeaked=true;','state.noticeLeaked=false;')]:
  directory=out/name;directory.mkdir(exist_ok=True);assert before in text
  (directory/'autoteste-tela.mjs').write_text(text.replace(before,after,1),encoding='utf-8')
  (directory/'w4-probe-tests.mjs').write_text((app/'testes/w4-probe-tests.mjs').read_text(encoding='utf-8'),encoding='utf-8')
  run=subprocess.run(['node',str(directory/'w4-probe-tests.mjs')],capture_output=True,text=True,timeout=10)
  (directory/'test.log').write_text(run.stdout+run.stderr,encoding='utf-8');assert run.returncode==1,name
  results.append(dict(case=name,detected=True));print(name+': detected',flush=True)
 # Deadline mutation runs the actual production camera host and a deliberately
 # blocked child; no driver/desktop permission is needed.
 for name,filename,before,after,expected in [('camera-opening-deadline','camera.cpp','GetTickCount64()+3000','GetTickCount64()+30000','camera blocked opening has hard deadline'),('camera-cancel-opening','camera.cpp','while(size&&!stop_&&GetTickCount64()<deadline)','while(size&&GetTickCount64()<deadline)','camera stop cancels blocked opening'),('camera-removed-default','camera.cpp','if(!stop_&&!id.empty()&&id!="__harmony_deadline_test__")','if(false)','camera removed selection falls back to default'),('camera-pixel-orientation','camera-conversion.h','stride<0?-height:height','height','camera conversion preserves orientation')]:
  directory=out/name;directory.mkdir(exist_ok=True)
  source=app.parent/'native/src'/filename;text=source.read_text(encoding='utf-8');assert before in text
  mutated=directory/filename;mutated.write_text(text.replace(before,after,1),encoding='utf-8')
  harness=app/'testes/camera-regressions.cpp'
  camera_source=mutated if filename=='camera.cpp' else app.parent/'native/src/camera.cpp'
  if filename.endswith('.h'):
   harness=directory/'camera-regressions.cpp';harness.write_text((app/'testes/camera-regressions.cpp').read_text(encoding='utf-8').replace('../../native/src/camera-conversion.h',str(mutated).replace('\\','/')).replace('../../native/src/camera.h',str(app.parent/'native/src/camera.h').replace('\\','/')).replace('../../native/src/net.h',str(app.parent/'native/src/net.h').replace('\\','/')),encoding='utf-8')
  run=subprocess.run(['cmd','/c',str(app/'camera-regressions.cmd'),str(camera_source),str(directory),str(harness)],capture_output=True,text=True,timeout=90)
  (directory/'test.log').write_text(run.stdout+run.stderr,encoding='utf-8');assert run.returncode==1 and expected in run.stderr and 'error C' not in run.stdout,name
  results.append(dict(case=name,detected=True));print(name+': detected',flush=True)
 text=(app.parent/'native/gpu/cadence.h').read_text(encoding='utf-8')
 for name,before,after in [('double-pacing','if (fps >= capture_fps) return true;','/* reintroduce second top pacing */'),('resolution-order','area == largest_area ? 0','area == largest_area ? 2')]:
  directory=out/name;directory.mkdir(exist_ok=True);header=directory/'cadence.h';assert before in text;header.write_text(text.replace(before,after),encoding='utf-8');code=cpp(name,header);assert code==1,name;results.append(dict(case=name,detected=True));print(name+': detected')
 text=(app.parent/'native/src/camera-policy.h').read_text(encoding='utf-8')
 for name,before,after in [('camera-portrait','720.0/height','720.0/width'),('camera-60-to-30','if(next&&now<next-16666)','if(false)'),('camera-format-choice','std::abs(fps-30)*30','0')]:
  directory=out/name;directory.mkdir(exist_ok=True);header=directory/'camera-policy.h';assert before in text;header.write_text(text.replace(before,after),encoding='utf-8');code=cpp(name,camera_header=header);assert code==1,name;results.append(dict(case=name,detected=True));print(name+': detected')
 text=(app/'testes/autoteste-metrics.mjs').read_text(encoding='utf-8')
 for name,before,after in [('compositor-acceptance',"average('composed')>=57","average('presented')>=57"),('late-fps',"if(!phase?.startsWith('high'))continue;","if(phase!=='high-initial')continue;"),('camera-frozen','s.camera.decoded>samples[i].camera.decoded','s.camera.decoded>0'),('late-sync','p.elapsed>start&&p.elapsed<=end','p.elapsed<60'),('received-rate',"delta('received')","(s.frames?.length??0)/seconds")]:
  directory=out/name;directory.mkdir(exist_ok=True);assert before in text;(directory/'autoteste-metrics.mjs').write_text(text.replace(before,after),encoding='utf-8');(directory/'w4-metrics-tests.mjs').write_text((app/'testes/w4-metrics-tests.mjs').read_text(encoding='utf-8'),encoding='utf-8');run=subprocess.run(['node',str(directory/'w4-metrics-tests.mjs')],capture_output=True,text=True);(directory/'test.log').write_text(run.stdout+run.stderr,encoding='utf-8');assert run.returncode==1,name;results.append(dict(case=name,detected=True));print(name+': detected')
(out/'results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
