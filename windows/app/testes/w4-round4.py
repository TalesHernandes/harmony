"""Replay the production WGC timestamp edge and game deadlines, including mutations."""
from pathlib import Path
import subprocess
import json
import sys

app = Path(__file__).resolve().parent.parent
native = app.parent / 'native'
out = app / 'test-output' / 'round4'
out.mkdir(parents=True, exist_ok=True)
capture = (native / 'gpu/capture.cpp').read_text(encoding='utf-8')
cadence = (native / 'gpu/cadence.h').read_text(encoding='utf-8')
# The production edge is two lines: the WGC frame time, then its conversion.
edge = '\n'.join(line.strip() for line in capture.splitlines()
                 if 'const int64_t source_ns = captured.SystemRelativeTime()' in line
                 or 'int64_t now = clock.Timestamp(' in line)
results = []

def run(name, header, timestamp):
    folder = out / name
    folder.mkdir(exist_ok=True)
    (folder / 'cadence.h').write_text(header, encoding='utf-8')
    source = r'''
#include "cadence.h"
#include <cstdio>
#include <initializer_list>
struct Frame {int64_t value;struct Time {int64_t value;int64_t count(){return value;}};
Time SystemRelativeTime(){return {value};}};
int64_t Timestamp(gpu::CaptureClock clock,Frame captured,int64_t dequeued) {
EDGE
return now;
}
int main() {
  gpu::CaptureClock clock{1000000000};
  auto a=Timestamp(clock,{10000000},2000000000);
  auto b=Timestamp(clock,{10166666},2066666600);
  bool failed=b-a!=16666600;
  if(failed)fprintf(stderr,"dequeue stall corrupted compositor timestamps\n");
  uint64_t slot=0;
  failed|=!gpu::GameDue(0,1000000,0,slot);
  failed|=!gpu::GameDue(100000,1000000,0,slot);
  if(gpu::GameDue(100001,1000000,0,slot)) {
    fprintf(stderr,"late game draw generated catch-up burst\n");failed=true;
  }
  failed|=!gpu::GameDue(116667,1000000,0,slot);
  for(int64_t frequency : {10000000LL,3579545LL}) {
    slot=0;
    for(int i=0;i<600;++i) {
      int64_t tick=(int64_t(i)*frequency+59)/60;
      failed|=!gpu::GameDue(tick,frequency,0,slot)||slot!=uint64_t(i+1);
      failed|=gpu::GameDue(tick+1,frequency,0,slot);
    }
  }
  return failed?1:0;
}
'''.replace('EDGE', timestamp)
    (folder / 'test.cpp').write_text(source, encoding='utf-8')
    built = subprocess.run(['cmd', '/c', str(native / 'vc.cmd'), 'cl', '/nologo', '/std:c++20',
                            '/EHsc', '/MT', '/utf-8', str(folder / 'test.cpp'),
                            '/Fo' + str(folder / 'test.obj'), '/Fe' + str(folder / 'test.exe')],
                           capture_output=True, text=True)
    (folder / 'build.log').write_text(built.stdout + built.stderr, encoding='utf-8')
    assert built.returncode == 0, 'Compile failure is not a detected regression'
    tested = subprocess.run([str(folder / 'test.exe')], capture_output=True, text=True)
    (folder / 'test.log').write_text(tested.stdout + tested.stderr, encoding='utf-8')
    return tested

assert run('baseline', cadence, edge).returncode == 0
if '--mutate' in sys.argv:
    for name, header, timestamp, expected in [
        ('dequeue-timestamp', cadence, 'int64_t now = dequeued;', 'dequeue stall corrupted'),
        ('game-catch-up', cadence.replace('slot = uint64_t((now - start) * 60 / frequency) + 1;',
                                         '++slot;'), edge, 'catch-up burst')]:
        tested = run(name, header, timestamp)
        assert tested.returncode == 1 and expected in tested.stderr, name
        results.append(dict(case=name, detected=True, exit=tested.returncode))
        print(name + ': detected', flush=True)
(out / 'results.json').write_text(json.dumps(results, indent=2), encoding='utf-8')
print('Round 4 compositor timestamps and game deadlines passed', flush=True)
