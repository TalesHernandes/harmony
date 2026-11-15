"""Offline speech regression through the production C++ adapter; stdlib only.

Fixtures remain in the owner's source tree: speech CC BY-NC 3.0, noise CC BY
3.0 (assets/README.md). They must never be copied into the distribution.
"""
from pathlib import Path
import argparse
import array
import hashlib
import json
import math
import os
import shutil
import subprocess
import wave

app = Path(__file__).resolve().parent.parent
parser=argparse.ArgumentParser(); parser.add_argument('--exe',type=Path); parser.add_argument('--measure-only',action='store_true')
args=parser.parse_args()
# Keep each mutation's PCM and metrics beside its own executable. A failing
# mutant must not overwrite the successful production run's evidence.
out = args.exe.resolve().parent if args.exe else app/'test-output/dfn'
out.mkdir(parents=True, exist_ok=True)
deps = Path(os.environ['USERPROFILE'])/'harmony-deps/dfn-0.5.6'
assets = deps.parent/'fontes/DeepFilterNet-0.5.6/assets'
directory = out/'ação com espaço'
directory.mkdir(exist_ok=True)
for name, size, digest in [
    ('df.dll',16678912,'4040ce3da1dd1c26e15d2615f49fc38a92566a0384a0c4f1d149b8fe852f320f'),
    ('DeepFilterNet3_onnx.tar.gz',7983136,'c94d91f70911001c946e0fabb4aa9adc37045f45a03b56008cb0c8244cb63616')]:
    data=(deps/name).read_bytes()
    if len(data)!=size or hashlib.sha256(data).hexdigest()!=digest: raise RuntimeError('DFN hash mismatch: '+name)
    shutil.copyfile(deps/name,directory/name)

def read(name):
    with wave.open(str(assets/name)) as w:
        assert w.getframerate()==48000 and w.getsampwidth()==2
        channels=w.getnchannels(); values=array.array('h',w.readframes(w.getnframes()))
    return [sum(values[i:i+channels])/(32768*channels) for i in range(0,len(values),channels)]

speech=read('clean_freesound_33711.wav')
noise=read('noise_freesound_2530.wav')[:len(speech)]
speech_energy=sum(x*x for x in speech)
noise_scale=math.sqrt(speech_energy/(sum(x*x for x in noise)*10**(5/10)))
noise=[v*noise_scale for v in noise]
mixture=[s+n for s,n in zip(speech,noise)]
scale=.85/max(abs(x) for x in mixture)
speech=[v*scale for v in speech]; noise=[v*scale for v in noise]
mixture=array.array('h',(round(v*scale*32768) for v in mixture))
# Tail accommodates analysis/synthesis delay. Never include it in the metric.
mixture.extend([0]*4800)
while len(mixture)%480: mixture.append(0)
incoming=out/'speech-mixture.pcm'; incoming.write_bytes(mixture.tobytes())

if args.exe:
    subprocess.run([str(args.exe.resolve()),str(directory.resolve())],check=True)
    target=out/'speech-output.pcm'
    subprocess.run([str(args.exe.resolve()),str(directory.resolve()),str(incoming.resolve()),str(target.resolve())],check=True)
    values=array.array('h',target.read_bytes()); output=[v/32768 for v in values]
    # Coarse then fine delay search, maximizing cross correlation on speech.
    # Decimation is ONLY for lag search. All metrics use every sample.
    def score(lag): return sum(speech[i]*output[i+lag] for i in range(48000,len(speech)-4800,24))
    coarse=max(range(0,3001,24),key=score)
    lag=max(range(max(0,coarse-24),coarse+25),key=score)
    x=speech[48000:-4800]; y=output[48000+lag:len(speech)-4800+lag]; n=noise[48000:-4800]
    xx=sum(v*v for v in x); yy=sum(v*v for v in y); xy=sum(a*b for a,b in zip(x,y)); nn=sum(v*v for v in n)
    gain=xy/xx; correlation=xy/math.sqrt(xx*yy)
    residual=sum((a-b)**2 for a,b in zip(y,x))
    projected=sum((a-gain*b)**2 for a,b in zip(y,x))
    # Separate voiced 10 ms windows (clean energy > 1% of peak window).
    energies=[sum(v*v for v in x[i:i+480]) for i in range(0,len(x),480)]
    voiced=[i for i,e in enumerate(energies) if e>max(energies)*.01]
    vx=vy=vxy=0
    for i in voiced:
        a=x[i*480:(i+1)*480]; b=y[i*480:(i+1)*480]
        vx+=sum(v*v for v in a); vy+=sum(v*v for v in b); vxy+=sum(c*d for c,d in zip(a,b))
    # Near-silent clean windows with 30 ms guards exclude speech/tail leakage.
    pauses=[i for i in range(3,len(energies)-3) if max(energies[i-3:i+4])<max(energies)*.0001]
    pause_input=pause_residual=0
    for i in pauses:
        pause_input+=sum(v*v for v in n[i*480:(i+1)*480])
        pause_residual+=sum((a-b)**2 for a,b in zip(y[i*480:(i+1)*480],x[i*480:(i+1)*480]))
    result=dict(input_snr_db=5,strength_db=50,alignment_samples=lag,
        residual_attenuation_db=10*math.log10(nn/residual),
        projected_residual_attenuation_db=10*math.log10(nn/projected),
        correlation=correlation,speech_gain=gain,voiced_correlation=vxy/math.sqrt(vx*vy),voiced_energy_ratio=vy/vx,
        pause_noise_attenuation_db=10*math.log10(pause_input/pause_residual),pause_windows=len(pauses),
        voiced_windows=len(voiced),fixture_hashes={name:hashlib.sha256((assets/name).read_bytes()).hexdigest() for name in ['clean_freesound_33711.wav','noise_freesound_2530.wav']},
        method='Align by speech correlation; unity-gain output minus clean speech includes both remaining noise and speech distortion. Projection also reported; unity residual is the regression criterion. Discard first second and final 100 ms. Voiced windows exceed 1% of peak clean window energy.')
    (out/'speech-metrics.json').write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result,indent=2),flush=True)
    if not args.measure_only:
        # Conservative floors derived from the pinned fixture run (see w3-dfn.md).
        assert result['pause_windows']>=150 and result['pause_noise_attenuation_db']>15, 'dfn real speech noise attenuation'
        assert result['residual_attenuation_db']>.2, 'dfn real speech noise attenuation'
        assert result['voiced_correlation']>.8, 'dfn preserves speech correlation'
        assert .4<result['voiced_energy_ratio']<1.5, 'dfn preserves speech energy'
