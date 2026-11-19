# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import os, subprocess, wave, struct, math, json, time
import argparse
parser=argparse.ArgumentParser(description='Compare the C analysis test driver with a retained worker executable.')
parser.add_argument('reference', type=Path)
parser.add_argument('probe', type=Path)
parser.add_argument('artifacts', type=Path, help='output directory outside the checkout')
args=parser.parse_args()
mod=Path(__file__).resolve().parents[2]
base=args.artifacts.resolve()
if base == mod or mod in base.parents:
    parser.error('place comparison artifacts outside the core checkout')
base.mkdir(parents=True, exist_ok=True)
old=args.reference.resolve()
new=args.probe.resolve()
voice=mod/'tests/analysis/fixtures/pvdi-voice.wav'
with wave.open(str(voice),'rb') as f: params=f.getparams();pcm=f.readframes(f.getnframes());rate=f.getframerate();frames=f.getnframes()*18
with wave.open(str(base/'repeated-voice.wav'),'wb') as f:f.setparams(params);f.writeframes(pcm*18)
count=math.ceil(frames/rate*2)+1
grid=b'PQTZ'+struct.pack('>IIIII',24,24+8*count,0,0x80000,count)+b''.join(struct.pack('>HHI',i%4+1,12000,i*500) for i in range(count))
(base/'grid.DAT').write_bytes(b'PMAI'+struct.pack('>II',12,12+len(grid))+grid)
def read_old(path):
 data=path.read_bytes(); assert data[:4]==b'TA03'
 gl,pl,vl,rate,frames,method=struct.unpack_from('>6I',data,4)
 tag=data[28+gl:28+gl+pl];count,mood=struct.unpack_from('>HH',tag,16);end=struct.unpack_from('>H',tag,26)[0]
 phrases=[]
 for i in range(count):
  row=tag[32+i*24:56+i*24]
  phrases.append([int.from_bytes(row[2:4],'big'),int.from_bytes(row[4:6],'big'),row[7],row[9],row[19],row[11],int.from_bytes(row[12:14],'big'),int.from_bytes(row[14:16],'big'),int.from_bytes(row[16:18],'big'),row[21],int.from_bytes(row[22:24],'big')])
 vocal=data[28+gl+pl:];assert vocal[:4]==b'PVDI'
 return dict(source_rate=rate,source_frames=frames,mood=mood,end_beat=end,phrases=phrases,vocals=list(vocal[24:]))
modes=[('sequential',p,v) for p in (1,2,3) for v in (1,2,3)] + [('concurrent',1,1),('concurrent',1,2),('concurrent',2,1)]
rows=[]
for i,(mode,phrase,vocal) in enumerate(modes):
 env=dict(os.environ,TRACK_ANALYSIS_STAGE_MODE=mode,TRACK_ANALYSIS_PHRASE_THREADS=str(phrase),TRACK_ANALYSIS_PVDI_THREADS=str(vocal),TRACK_ANALYSIS_PVDI_MODEL=str(mod/'assets/analysis/pvdi-model.bin'))
 for kind,cmd in [('old',[str(old),str(base/'repeated-voice.wav'),str(base/'grid.DAT'),'120',str(base/f'old-{i}.TA03'),'-','40']),('c',[str(new),str(base/'repeated-voice.wav'),str(mod/'assets/analysis'),'40',str(base/f'c-{i}.json')])]:
  env['TRACK_ANALYSIS_PVDI_FEATURES']=str(base/f'{kind}-{i}.features');env['TRACK_ANALYSIS_PVDI_PROBABILITIES']=str(base/f'{kind}-{i}.probabilities')
  start=time.monotonic(); r=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=180)
  (base/f'{kind}-{i}.log').write_bytes(r.stdout+r.stderr)
  assert r.returncode==0,(kind,i,r.stderr.decode())
  elapsed=time.monotonic()-start; rows.append(dict(kind=kind,mode=mode,phrase_threads=phrase,vocal_threads=vocal,wall_s=elapsed))
  print(kind,i,round(elapsed,3),flush=True)
 assert read_old(base/f'old-{i}.TA03')==json.loads((base/f'c-{i}.json').read_text()),('typed result',i)
 for suffix in ['features','probabilities']:
  a=(base/f'old-{i}.{suffix}').read_bytes();b=(base/f'c-{i}.{suffix}').read_bytes()
  assert a==b,(suffix,i,len(a),len(b),next((j for j,(x,y) in enumerate(zip(a,b)) if x!=y),None))
 print('Full retained-worker parity:',mode,phrase,vocal,flush=True)
(base/'retained-comparison.json').write_text(json.dumps(rows,indent=2)+'\n')
