# SPDX-License-Identifier: GPL-3.0-only
from pathlib import Path
import json, math, subprocess
import sys
root=Path(__file__).resolve().parents[1]
path=Path(sys.argv[1])
fixture=Path(__file__).resolve().parent/'fixtures'
decls=[]; cases=[]; serial=0

def number(x):
 if isinstance(x,str): return {'nan':'NAN','inf':'INFINITY','-inf':'-INFINITY'}.get(x,x)
 if math.isnan(x): return 'NAN'
 if math.isinf(x): return 'INFINITY' if x>0 else '-INFINITY'
 return repr(x)
def arr(values,t='D'):
 global serial
 serial+=1; name=f'a{serial}'
 ctype={'D':'double','I':'int','B':'uint8_t'}[t]
 decls.append(f'static {ctype} {name}[]={{'+','.join(map(number,values or [0]))+'};')
 return f'({t}){{{name},{len(values)}}}'
def spectra(rows):
 global serial
 r=[arr(x) for x in rows]; serial+=1; name=f'rows{serial}';decls.append('static D '+name+'[]={'+','.join(r)+'};')
 return f'(Spectra){{{name},{len(rows)}}}'
def case(label,code): cases.append('{ label="'+label+'"; '+code+' ta_cleanup(c); }')
def load(name): return json.loads((fixture/(name+'.json')).read_text())
for i,w in enumerate(load('NativePhraseSpectra')['peaks']):
 code=f'Peaks p=spectral_peaks(c,{arr(w["input"])}); expect(p.n=={len(w["position"])});'
 for field in ['position','height','width','prominence']:
  e=arr(w[field]); code+=f'D {field}={e}; for(int j=0;j<p.n;j++) near(p.v[j].{field},{field}.v[j],1e-10);'
 case(f'peaks {i}',code)
for i,w in enumerate(load('NativePhraseSpectra')['vocal']): case(f'vocal spectra {i}',f'checkd(vocal_activity(c,{spectra(w["spectra"])}),{arr(w["expected"])},1e-10);')
for i,w in enumerate(load('NativePhraseSpectra')['vocalEntries']): case(f'vocal entries {i}',f'checki(vocal_entries(c,{arr(w["input"])}),{arr(w["expected"],"I")});')
f=load('NativePhraseClassifier')
for i,w in enumerate(f['mood']):
 fields=','.join('.'+k+'='+number(v) for k,v in w['input'].items())
 case(f'mood {i}',f'expect(initial_mood((MoodInputs){{{fields}}})=={w["expected"]});')
for i,w in enumerate(f['bpm']): case(f'bpm {i}',f'int a,b; bpm_flags({w["bpm"]},&a,&b); expect(a=={w["expected"][0]} && b=={w["expected"][1]});')
for i,w in enumerate(f['slowRock']): case(f'slow rock {i}',f'expect(slow_rock(c,{arr(w["low"],"B")},{arr(w["middle"],"B")})=={w["expected"]});')
for i,w in enumerate(f['rhythmFlags']):
 code=f'RhythmFlags f=rhythm_flags(c,{arr(w["rhythm"],"B")},{w["bpm"]},{arr(w["blockPowers"])});'
 for k,v in zip(['fourKick','popRock','alternateRock','sparseKick'],w['expected']):code+=f'expect(f.{k}=={v});'
 case(f'rhythm flags {i}',code)
for i,w in enumerate(f['power']):
 case(f'power {i}',f'LowPower p=classify_power(c,{arr(w["low"])},{arr(w["full"])},{w["beatCount"]}); near(p.ratio,{w["expected"][0]},1e-11); near(p.ratioMean,{w["expected"][1]},1e-11); near(p.powerMean,{w["expected"][2]},1e-11); checkd(p.ratios,{arr(w["blockRatios"])},1e-11); checkd(p.powers,{arr(w["blockPowers"])},1e-11);')
for i,w in enumerate(f['compression']):case(f'compression {i}',f'Compression p=classify_compression(c,{arr(w["low"])},{arr(w["full"])},{arr(w["rhythm"],"B")},{w["phase"]}); near(p.first,{w["expected"][0]},1e-11); near(p.second,{w["expected"][1]},1e-11);')
for i,w in enumerate(f['slowTempo']):
 onset=f'(SnarePositions){{.values={arr(w["onsetSamples"],"I")},.count={w["onsetCount"]},.first={w["firstOnset"]}}}'
 case(f'slow tempo {i}',f'expect(slow_tempo(c,{w["sampleRate"]},{w["bpm"]},{arr(w["blockPowers"])},{onset},{arr(w["beatSamples"],"I")},{w["sourceOffsetMS"]},{w["analysisOffsetMS"]})=={w["expected"][0]});')
for i,w in enumerate(f['seeds']): case(f'seeds {i}',f'SeedEvidence p=non_high_seeds(c,{arr(w["middleLevels"])},{arr(w["openingEnvelope"])},{arr(w["boundaries"],"I")},{arr(w["vocalActivity"])},{w["mood"]}); checki(p.roles,{arr(w["expected"],"I")});')
for i,w in enumerate(f['recurrence']):
 code=f'Recurrence r=recurrence(c,{spectra(w["spectra"])},{arr(w["boundaries"],"I")},{w["phase"]},{w["endBeat"]});'
 for row,values in enumerate(w['expected']):
  for col,v in enumerate(values):
   if col>row: code+=f'expect(r.related[{row}].v[{col}]=={v});'
 case(f'recurrence {i}',code)
for i,w in enumerate(f['roles']):
 rows=[arr(x,'B') for x in w['related']]; serial+=1; name=f'rel{serial}'
 decls.append('static B '+name+'[]={'+','.join(rows)+'};')
 case(f'roles {i}',f'Recurrence r={{.related={name},.n={len(rows)}}}; checki(reconcile_roles(c,{arr(w["roles"],"I")},r),{arr(w["expected"],"I")});')
for i,row in enumerate(load('NativePhraseTiming')['records']):
 if row['mode'] not in ('rhythm','timing-rhythm'):continue
 w=row['input']; e=row['expected']
 code=f'Attacks a=attacks_init(c,{w["rate"]},{int(w["second"])}); a.delay={w["delay"]}; a.previous={w["previous"]}; a.maximum={w["maximum"]};'
 if row['mode']=='timing-rhythm': code+=f'B p=attacks_timing(c,&a,{arr(w["input"])},{w["rate"]},{w["bpm"]},{w["offset"]},{w["phase"]},{w["block"]});'
 else: code+=f'B p=attacks_process(c,&a,{arr(w["input"])},{w["rate"]},{w["bpm"]},{w["offset"]},{w["phase"]},{w["block"]},{arr(w["coordinates"],"I")},{w["count"]},{int(row["mode"]=="timing-rhythm")});'
 code+=f'checkb(p,{arr(e["pattern"],"B")}); near(a.previous,{e["previous"]},1e-12); near(a.maximum,{e["maximum"]},1e-12); expect(a.delay=={e["delay"]});'
 case(f'timing rhythm {i}',code)
for kind in ['judge','main']:
 for i,w in enumerate(load('NativePhraseOpening')[kind]):
  args=','.join(arr(w[k]) for k in ['envelope','low','middle','vocal'])+','+str(w['phase'])
  if kind=='main':args+=','+arr(w['anchors'],'I')
  code=f'Opening r=opening_{kind}(c,{args}); checki(r.hypotheses,{arr(w["expected"],"I")}); expect(r.selected=={w["selected"]} && r.found=={w["flag"]});'
  if kind=='judge':code+=f'checki(r.raw,{arr(w["hypotheses"],"I")});'
  case(f'opening {kind} {i}',code)
for i,w in enumerate(load('NativePhraseRoute')['route']):
 case(f'route {i}',f'Route r=phrase_route(c,{arr(w["low"])},{w["phase"]},{w["duration"]},{arr(w["loop"],"I")},{arr(w["rhythm"],"I")},{w["mood"]}); expect(r.mood=={w["expectedMood"]} && r.route=={w["expectedRoute"]}); checkd(r.positive,{arr(w["profiles"][1])},1e-11); checkd(r.negative,{arr(w["profiles"][2])},1e-11);')
for i,w in enumerate(load('NativePhraseHigh')):
 case(f'high {i}',f'HighRoles r=high_classify(c,{arr(w["boundaries"],"I")},{arr(w["intro"])},{arr(w["low"])}); checki(r.roles,{arr(w["roles"],"I")}); expect(r.simple=={int(w["simple"])});')
for i,w in enumerate(load('NativePhraseExport')['variation']):
 case(f'variation {i}',f'Variation r=phrase_variation(c,{arr(w["envelope"])},{arr(w["boundaries"],"I")}); expect(r.flag=={w["flag"]}); near(r.summary,{w["summary"]},1e-11);')
for i,w in enumerate(load('NativePhraseNonHigh')['nonHigh']):
 code=f'PhraseExport r=export_non_high(c,{w["mood"]},{arr(w["boundaries"],"I")},{spectra(w["recurrenceSpectra"])},{w["phase"]},{w["endBeat"]},{arr(w["middleLevels"])},{arr(w["openingEnvelope"])},{arr(w["vocalActivity"])},{arr(w["lowPeaks"])},{arr(w["activity"],"B")},{arr(w["lowQuarters"])},{arr(w["powerQuarters"])}); expect(r.map.end_beat=={w["expectedTerminal"]}); expect(r.variation.flag=={w["variationFlag"]}); near(r.variation.summary,{w["variationSummary"]},1e-11);'
 code+=f'I expected={arr(w["expectedKinds"],"I")}; expect(r.map.count==(unsigned)expected.n); for(int j=0;j<expected.n;j++)expect(r.map.entries[j].kind==(unsigned)expected.v[j]);'
 case(f'non high {i}',code)
for i,w in enumerate(load('NativePhraseFills')['records']):
 mode=w['mode']; code=''
 if mode in ('cut','growth'):
  args=f'1,{w["end"]},{arr(w["low"])}'+(','+arr(w['fine']) if mode=='cut' else '')
  code=f'FillDetection r=fill_{mode}(c,{args}); expect(r.start=={w["expected"][0]} && r.end=={w["expected"][1]});'
 elif mode=='upper':code=f'I s=repeated_scores(c,(HighProfile){{{arr(w["values"])},{w["supported"]}}}); FillDetection r=repeated_upper({w["end"]},1,s);expect(r.start=={w["expected"][0]} && r.end=={w["expected"][1]});'
 elif mode=='highNormalize':code=f'HighProfile r=normalize_high(c,{arr(w["values"])},{w["phase"]}); expect(r.beats=={w["supported"]});checkd(r.values,{arr(w["expected"])},1e-11);'
 elif mode=='beatFour':code=f'expect(beat_four(c,{w["start"]},{w["end"]},{int(w["last"])},{arr(w["low"])},{arr(w["quarters"])})=={int(w["expected"])});'
 elif mode=='snare':code=f'checki(snare_candidates(c,{arr(w["values"])},{w["bars"]}),{arr(w["expected"],"I")});'
 elif mode=='loopRestore':code=f'checki(restore_loops(c,{arr(w["roles"],"I")},{arr(w["saved"],"I")},{arr(w["metric"])}),{arr(w["expected"],"I")});'
 elif mode=='simpleLoop':code=f'UpAnalysis r=simple_loops(c,{arr(w["boundaries"],"I")},{arr(w["roles"],"I")},{arr(w["first"])},{arr(w["second"])},{arr(w["metric"])}); checki(r.roles,{arr(w["expectedRoles"],"I")});checki(r.starts,{arr(w["expectedStarts"],"I")});checkd(r.metrics,{arr(w["expectedMetric"])},1e-11);'
 elif mode=='rhythm':
  code=f'RhythmAnalysis r=decorate_rhythm(c,{arr(w["boundaries"],"I")},{arr(w["roles"],"I")},{arr(w["activity"],"B")});'
  for field,values in zip(['roles','starts','first','second','third'],w['expected']):code+=f'checki(r.{field},{arr(values,"I")});'
 elif mode=='outro':code=f'Outro r=phrase_outro(c,{arr(w["boundaries"],"I")},{arr(w["roles"],"I")},{arr(w["low"])},{arr(w["activity"],"B")},{arr(w["quarters"])},{w["phase"]});expect(r.terminal=={w["expectedTerminal"]});checki(r.roles,{arr(w["expectedRoles"],"I")});'
 elif mode in ('final','cleanup'):
  n=len(w['roles']);code=f'Fills r={{i_copy(c,{arr(w["roles"],"I")}),i_copy(c,{arr(w["starts"],"I")}),ta_new_ints(c,{n}),ta_new_ints(c,{n})}};'
  if mode=='final':code+=f'final_adjust(c,{arr(w["boundaries"],"I")},{int(w["high"])},{arr(w["metric"])},&r);'
  else:code+=f'clean_activity(c,{arr(w["boundaries"],"I")},(FillProfile){{.low={arr(w["low"])},.power={arr(w["power"])},.phase=1,.quarters={{0}} }},&r);checki(r.auxiliary,{arr(w["expectedAuxiliary"],"I")});'
  code+=f'checki(r.roles,{arr(w["expectedRoles"],"I")});checki(r.starts,{arr(w["expectedStarts"],"I")});'
 elif mode=='high':
  profile=f'(FillProfile){{{arr(w["low"])},{arr(w["fine"])},{arr(w["high"])},{arr(w["power"])},{w["phase"]},{arr(w["quarters"])}}}'
  code=f'Decorated r=decorate_high(c,{arr(w["boundaries"],"I")},{arr(w["roles"],"I")},{profile},{arr(w["first"])},{arr(w["second"])},{arr(w["metric"])},{arr(w["activity"],"B")});expect(r.boundaries.v[r.boundaries.n-1]=={w["expectedTerminal"]});'
  for field,values in zip(['fills.roles','fills.starts','fills.auxiliary','upStarts','upFirst','upSecond','upThird'],w['expected']):code+=f'checki(r.{field},{arr(values,"I")});'
 elif mode=='openingFeatures':
  code=f'OpeningFeatures r=opening_features(c,{spectra(w["spectra"])},{w["firstFrame"]},{w["beatCount"]});checkb(r.presence,{arr(w["expected"][0],"B")});checkb(r.rises,{arr(w["expected"][1],"B")});checkd(r.envelope,{arr(w["expected"][2])},1e-11);'
 elif mode=='openingSpectra':
  code=f'Spectra r=opening_spectra(c,{arr(w["samples"])},{w["rate"]});expect(r.n=={len(w["expected"])});'
  for j,row in enumerate(w['expected']):code+=f'checkd(r.rows[{j}],{arr(row)},1e-10);'
 if code:case(f'fills {mode} {i}',code)
for i,w in enumerate(load('NativePhraseExport')['conversion']):
 n=len(w['roles']);zeros=arr([0]*n,'I')
 code=f'I b=ta_new_ints(c,{n+1}); Fills fills={{{arr(w["roles"],"I")},{zeros},{arr(w["auxiliary"],"I")},{zeros}}}; Decorated d={{b,fills,{arr(w["up"][3],"I")},{arr(w["up"][0],"I")},{arr(w["up"][1],"I")},{arr(w["up"][2],"I")}}}; PhraseExport r=phrase_convert(c,{w["mood"]},d,(Variation){{0}});checki(r.auxiliary,{arr(w["expectedAuxiliary"],"I")});'
 for field,values in zip(['kind','variant1','variant2','structure','beat2','beat3','beat4','variant3','fill'],w['fields'][1:10]):
  code+=f'I e_{field}={arr(values,"I")}; for(int j=0;j<{n};j++)expect(r.map.entries[j].{field}==(unsigned)e_{field}.v[j]);'
 case(f'conversion {i}',code)
# Generated-profile expectations retained from independent native boundary execution.
boundary_expectations = [[1, 17, 33, 49, 81, 113, 129, 161, 177, 209], [1, 18, 34, 50, 66, 98, 114, 146, 162, 194, 226], [1, 19, 51, 83, 99, 131, 163, 179, 211], [1, 20, 36, 68, 80, 112, 116, 148, 160, 192, 208], [1, 17, 33, 49, 65, 81, 97, 113, 145, 161, 177, 193, 209, 225], [1, 18, 26, 50, 66, 82, 98, 130, 146, 178, 194, 210], [1, 35, 51, 67, 115, 131, 143, 163, 179, 195], [1, 20, 28, 36, 48, 100, 112, 128, 148, 164, 176, 228], [1, 17, 33, 49, 81, 97, 113, 129, 145, 161, 177, 209, 225], [1, 18, 26, 34, 66, 82, 114, 130, 146, 162, 194, 210], [1, 27, 51, 67, 79, 99, 115, 131, 179, 195, 207, 227], [1, 36, 48, 64, 84, 100, 112, 164, 176, 192, 212, 228], [1], [1], [1], [1]]
for sample,expected in enumerate(boundary_expectations):
 phase=sample%4+1;count=256
 def profile(pattern):return [pattern[(i//16+sample)%len(pattern)] for i in range(count)]
 peaks=profile([0.03,0.04,0.82,0.86,0.84,0.3,0.29,0.7])
 changes=[0.0]+[1 if peaks[i]>peaks[i-1]*1.77 else 0.5 if peaks[i-1]>peaks[i]*2 else 0 for i in range(1,count)]
 low=arr(profile([20,30,900,950,910,180,170,800]));middle=arr(profile([90,200,70,500,800,140,370,800]));all=arr(profile([0.03,0.08,0.7,0.8,0.65,0.9,0.5,0.8]))
 mp=arr(profile([0.01,0.1,0.8,0.7,0.6,0.05,0.6,0.9]));opening=arr(profile([0.01,0.04,0.6,0.7,0.8,0.2,0.3,0.5]));vocal=arr(profile([0,0,1,1,0,0,1,1]))
 high=arr([0.8 if (i+sample)%8==3 else 0.02 for i in range(count*8)]);bars=(count-phase+1)//4
 hh=arr([int((i+sample)%3==0) for i in range(bars)],'B');rhythm=arr([int((i+sample)%4==0) for i in range(bars*16)],'B')
 code=f'Beats b={{0}}; b.timing=(Timing){{128,0,{phase}}}; b.coordinates.n=256; b.initial=1; b.lowLevels={low}; b.middleLevels={middle}; b.allLevels={all}; b.lowPeaks={arr(peaks)}; b.lowPeakChanges={arr(changes)}; b.middlePeaks={mp}; b.eighths={high}; b.lowActivity=low_activity(c,b.lowLevels,{phase}); b.middleActivity=middle_activity(c,b.middleLevels,{phase}); b.allActivity=all_activity(c,b.allLevels,{phase});'
 code+=f'Opening o={{.hypotheses={arr([phase+i*4 for i in [3,4,5,6]],"I")},.selected={phase+16},.found={int(sample<12)}}}; checki(phrase_candidates(c,b,o,{opening},{vocal},{hh},{rhythm},120,{sample//4+1 if sample<12 else 1}),{arr(expected,"I")});'
 case(f'complete boundaries {sample}',code)
source='''#include "phrase.h"
#include <assert.h>
static const char *label;
static void expect(int ok) { if(!ok) { fprintf(stderr,"failed: %s\\n",label); abort(); } }
static void near(double a,double b,double tolerance) { if(isnan(a) && isnan(b))return; if(isinf(a) && isinf(b) && signbit(a)==signbit(b))return; if(!(fabs(a-b)<=fmax(tolerance,fabs(b)*tolerance))) { fprintf(stderr,"%s: %.17g expected %.17g\\n",label,a,b); abort(); } }
static void checkd(D a,D b,double tolerance) { expect(a.n==b.n); for(int i=0;i<a.n;i++) near(a.v[i],b.v[i],tolerance); }
static void checki(I a,I b) { expect(a.n==b.n); for(int i=0;i<a.n;i++) expect(a.v[i]==b.v[i]); }
static void checkb(B a,B b) { expect(a.n==b.n); for(int i=0;i<a.n;i++) expect(a.v[i]==b.v[i]); }
'''+ '\n'.join(decls)+'''\nint main(void) { ta_context *c=calloc(1,sizeof *c); assert(c); if(setjmp(c->failure)) { fprintf(stderr,"%s: %s\\n",label,c->error);ta_cleanup(c);free(c);return 1; }\n'''+ '\n'.join(cases)+f'free(c); puts("{len(cases)} original phrase witnesses passed in C");return 0;}}\n'
path.parent.mkdir(parents=True,exist_ok=True)
path.write_text(source)
