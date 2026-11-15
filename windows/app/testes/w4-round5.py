from pathlib import Path
import subprocess,sys,json
app=Path(__file__).resolve().parent.parent
out=app/'test-output'/'round5';out.mkdir(parents=True,exist_ok=True)
results=[]
def guard(directory,header=None):
 directory.mkdir(parents=True,exist_ok=True)
 source=(app/'testes/w4-guard-fixture.cpp').read_text(encoding='utf-8-sig')
 source=source.replace('"../src/test-process-guard.h"','"'+str(header or app/'src/test-process-guard.h').replace('\\','/')+'"')
 fixture=directory/'fixture.cpp';fixture.write_text(source,encoding='utf-8')
 exe=directory/'fixture.exe'
 build=subprocess.run(['cmd','/c',str(app.parent/'native/vc.cmd'),'cl','/nologo','/std:c++20','/EHsc','/MT','/utf-8',str(fixture),'/Fo'+str(directory/'fixture.obj'),'/Fe'+str(exe)],capture_output=True,text=True)
 (directory/'build.log').write_text(build.stdout+build.stderr,encoding='utf-8');assert build.returncode==0,build.stdout
 return subprocess.run(['node',str(app/'testes/w4-guard-tests.mjs'),str(exe)],capture_output=True,text=True,timeout=25)
if '--mutate' not in sys.argv:
 run=guard(out/'guard');print(run.stdout,run.stderr);assert run.returncode==0
 for test in ['w4-surface-tests.mjs','w4-process-tests.mjs']:subprocess.run(['node',str(app/'testes'/test)],check=True)
else:
 cases=[('surface-follow','autoteste-surface.mjs','current!==video||current.srcObject!==stream','!video','w4-surface-tests.mjs'),('surface-deadline','autoteste-surface.mjs','},timeout);','},timeout*20);','w4-surface-tests.mjs'),('surface-compositor','autoteste-surface.mjs','base+=last;last=0;','base+=0;last=0;','w4-surface-tests.mjs'),('process-preflight','autoteste-processes.mjs','if(live.length)','if(false)','w4-process-tests.mjs'),('process-tree','autoteste-processes.mjs',"'/T','/F'","'/F'",'w4-process-tests.mjs'),('compositor-acceptance','autoteste-metrics.mjs',"average('composed')>=57","average('presented')>=57",'w4-metrics-tests.mjs')]
 for name,file,before,after,test in cases:
  directory=out/name;directory.mkdir(exist_ok=True);text=(app/'testes'/file).read_text(encoding='utf-8');assert before in text
  (directory/file).write_text(text.replace(before,after,1),encoding='utf-8');(directory/test).write_text((app/'testes'/test).read_text(encoding='utf-8-sig'),encoding='utf-8')
  run=subprocess.run(['node',str(directory/test)],capture_output=True,text=True,timeout=15)
  (directory/'test.log').write_text(run.stdout+run.stderr,encoding='utf-8');assert run.returncode==1,name
  results.append(dict(case=name,detected=True));print(name+': detected',flush=True)
 for name,before,after in [('parent-watch','WaitForSingleObject(parent,INFINITE);TerminateJobObject(job,1);','Sleep(INFINITE);'),('job-tree','!AssignProcessToJobObject(job,GetCurrentProcess())','false')]:
  directory=out/name;directory.mkdir(exist_ok=True);text=(app/'src/test-process-guard.h').read_text(encoding='utf-8');assert before in text
  header=directory/'test-process-guard.h';header.write_text(text.replace(before,after,1),encoding='utf-8')
  run=guard(directory,header);(directory/'test.log').write_text(run.stdout+run.stderr,encoding='utf-8');assert run.returncode==1 and 'parent disappearance must kill fixture and its child' in run.stderr,name
  results.append(dict(case=name,detected=True));print(name+': detected',flush=True)
 (out/'mutation-results.json').write_text(json.dumps(results,indent=2),encoding='utf-8')
