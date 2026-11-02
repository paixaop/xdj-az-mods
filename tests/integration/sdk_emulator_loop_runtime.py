#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Owned full-firmware public loop-command integration, with actual UI trust."""
from sdk_timestamps import compare_timestamps
from pathlib import Path
import hashlib,io,json,math,re,tarfile,time,uuid
import sdk_emulator_sdk as runtime
from sdk_emulator_public_runtime import run_public_runtime
from sdk_emulator_public import probe_archive,read_script
from sdk_emulator_loaded import run_observed_runtime,native_birth_check_script
from sdk_panel_transport import PanelTransport,PanelTransportSessions
from sdk_emulator_controls import MissingEvidence,position_motion
from sdk_emulator_loop import ready
from sdk_emulator_loop_continuation import run_continuation
from sdk_emulator_loop_batch import run_batch_commands as run_commands,run_batch_refusals as run_refusals
from sdk_runtime_check import validate_health
ID='sdk-loop-command-probe'
LOG='/tmp/azmod-community/'+ID+'.log'
def sha(path):return hashlib.sha256(Path(path).read_bytes()).hexdigest()

def run_continuation_after_panel_close(controller,sessions,*args,**kwargs):
 """End the acknowledged physical-input lease before SDK-only observation."""
 if not controller.panel_inputs_neutral():raise MissingEvidence('panel inputs must be neutral before closing the receiver')
 sessions.close()
 return run_continuation(*args,**kwargs)

def ensure_quantize_off(controller):
 """Use the real global input and fresh public state for a nonquantized matrix."""
 before=controller.observe()
 def bit(row):
  value=row.get('quantize')
  if type(value) is int and value in (0,1):return value
  if type(value) is str and value in ('0','1'):return int(value)
  raise MissingEvidence('valid global Quantize state unavailable for loop setup')
 rows=[]
 for deck in range(1,5):
  try:rows.append(controller._stream(before,deck,'quantize'))
  except MissingEvidence:rows.append(None)
 known=[bit(row) for row in rows if row is not None]
 if len(set(known))>1:raise MissingEvidence('uniform global Quantize state unavailable for loop setup')
 def unit(row):
  value=row.get('unit')
  if type(value) not in (int,str):raise MissingEvidence('valid Quantize unit unavailable')
  try:number=int(value)
  except (ValueError,TypeError):raise MissingEvidence('valid Quantize unit unavailable')
  if not 0<number<2**64 or (type(value) is str and str(number)!=value):raise MissingEvidence('valid Quantize unit unavailable')
  return number
 units=[unit(row) if row is not None else None for row in rows]
 if any(row is None for row in rows):
  # Registration does not replay an unchanged setting on a warm restart.
  # Establish its first real receipt once; never guess the initial value.
  key=(0,'quantize')
  if key in controller.setup_attempts:raise MissingEvidence('global Quantize observation setup already attempted')
  fence=controller._native_now(before,1)
  if fence is None:raise MissingEvidence('fresh native clock required for Quantize observation setup')
  controller.setup_attempts.add(key)
  controller.quantize()
  def received(value):
   try:observed=[controller._stream(value,deck,'quantize') for deck in range(1,5)]
   except MissingEvidence:return False
   now=controller._native_now(value,1)
   if now is None:return False
   values=[bit(row) for row in observed]
   if values not in ([0]*4,[1]*4):return False
   for previous,row in zip(units,observed):
    current=unit(row)
    if previous is not None and current!=previous:raise ValueError('native unit changed during Quantize observation setup')
    stamp=int(row['received_ns'])
    if compare_timestamps(stamp,now+10000000)>0:raise ValueError('future Quantize setup receipt')
    if int(row['revision'])<=0 or compare_timestamps(stamp,fence)<=0:return False
   return True
  before=controller.wait(received,'first all-deck Quantize receipt after one setup input',allow_partial=True)
  if not received(before):raise MissingEvidence('global Quantize receipt unavailable after one setup input')
  rows=[controller._stream(before,deck,'quantize') for deck in range(1,5)]
 values=[bit(row) for row in rows]
 if values==[0]*4:return
 if values!=[1]*4:raise MissingEvidence('uniform global Quantize state unavailable for loop setup')
 controller.quantize()
 def changed(value):
  return all(controller._newer(rows[deck-1],controller._stream(value,deck,'quantize')) and
             bit(controller._stream(value,deck,'quantize'))==0 for deck in range(1,5))
 after=controller.wait(changed,'all-deck Quantize off for loop commands',allow_partial=True)
 if not changed(after):raise MissingEvidence('all-deck Quantize off transition unavailable')

def wait_playback_motion(controller,logical,snapshot):
 """Require actual fresh position advances after the single setup PLAY."""
 packet=snapshot.get('positions')
 if not packet or not packet.get('samples'):raise MissingEvidence('loaded source position unavailable before PLAY')
 initial=packet['samples'][-1]['decks'][logical-1]
 start=initial.get('pos_s')
 if type(start) not in (int,float) or not math.isfinite(start):raise MissingEvidence('loaded source position unavailable before PLAY')
 fence=packet['native_monotonic_ns']
 def advanced(value):
  proof=position_motion(value.get('positions'),logical,controller.pid,controller.position_binding,
                        after_ns=fence,now_boottime_ns=value['boottime_ns'])
  if not proof.get('binding_verified') or not proof.get('fresh'):return False
  # Positive movement can be observed across sparse samples. The existing
  # classifier's 250ms bound is required to rule out aliased stationary loops,
  # not to witness actual forward advances during this loop-free setup.
  rows=[sample['decks'][logical-1] for sample in value['positions']['samples']
        if compare_timestamps(sample['native_monotonic_ns'], fence) > 0]
  if len(rows)<3 or any(row.get('read_rc')!=0 or row.get('loaded')!=1 or
       row.get('title')!=initial.get('title') or type(row.get('pos_s')) not in (int,float) or not math.isfinite(row['pos_s']) for row in rows):return False
  steps=[b['pos_s']-a['pos_s'] for a,b in zip(rows,rows[1:])]
  rate=controller.position_binding['engine_rate']
  return all(compare_timestamps(delta,0.,units_per_second=1,sample_rate=rate)>=0 for delta in steps) and sum(compare_timestamps(delta,0.,units_per_second=1,sample_rate=rate)>0 for delta in steps)>=2 and compare_timestamps(rows[-1]['pos_s'],start+0.5,units_per_second=1,sample_rate=rate)>=0
 return controller.wait(advanced,'deck%d playback source advances after PLAY'%logical)

def run_loop_runtime(ssh,package,source_root,output,firmware,seconds,command,context):
 """Run a normal-loader command client in an explicitly owned disposable VM.

 prepare_allow(controller,read_guest,sessions) performs actual UI Allow/reload.
 prepare_deck(controller,logical_deck) selects a test track and returns a PASS
 selection with exact selected_title and retained screenshot evidence. This
 adapter, rather than the callback, issues the single LOAD and setup PLAY.
 """
 PanelTransport._strict_ssh(ssh)
 owned=dict(context)
 debug=context.get('debug',False)
 if type(debug) is not bool:raise ValueError('debug must be an explicit boolean')
 if context.get('on_failure') is not None and not callable(context['on_failure']):raise ValueError('on_failure callback must be callable')
 if not callable(context.get('prepare_allow')) or not callable(context.get('prepare_deck')):
  raise ValueError('normal Allow and per-deck selection callbacks required')
 if not re.fullmatch('[a-f0-9]{32}',str(owned.get('token',''))) or not re.fullmatch('az-sdk-automation-[a-z0-9-]+',str(owned.get('container',''))):
  raise ValueError('one owned disposable emulator required')
 if type(seconds) is not int or not 240<=seconds<=1500:raise ValueError('bounded normal-loader runtime required')
 manifest=runtime.validate_package(package,source_root)
 DRIVER=Path(context['driver_path']);RECEIVER=Path(context['receiver_path'])
 for path,key in ((DRIVER,'driver_sha256'),(RECEIVER,'receiver_sha256')):
  if path.is_symlink() or not path.is_file() or sha(path)!=context[key]:raise ValueError('bound driver/receiver required')
 titles=context.get('expected_titles')
 if not isinstance(titles,(list,tuple)) or not 1<=len(titles)<=8 or any(not isinstance(x,str) or not 1<=len(x)<=512 for x in titles):raise ValueError('explicit exact test titles required')
 usb=context.get('usb_mount','/media/usb/sda1')
 if not re.fullmatch('/media/usb/[A-Za-z0-9_-]+',usb):raise ValueError('one guest USB mount required')
 output=Path(output)
 if output.exists() or output.is_symlink():raise ValueError('new loop runtime evidence output required')
 binding=json.loads((Path(package)/'runtime.json').read_text())['position_binding']
 probe_manifest=Path(context['probe_manifest']);pb=json.loads(probe_manifest.read_text());probe_package=Path(context['probe_package'])
 if context.get('position_binding')!=binding or sha(Path(source_root)/'src/azmod/azmod-mod.h')!=pb['sha256']['include/azmod-mod.h']:raise ValueError('public header/native provider binding mismatch')
 files=probe_archive(probe_package.read_bytes(),pb,mod_id=ID)
 def owner():
  value=command(['docker','inspect','--format','{{index .Config.Labels "az-sdk-token"}}',owned['container']],timeout=10).decode().strip()
  if value!=owned['token']:raise ValueError('owned VM identity changed')
  return value
 owner()
 # Real USB metadata supplies the genuine core data_dir. No RAM fallback.
 target=usb+'/MODS/community-mods/'+ID
 stage=usb+'/MODS/community-mods/.sdk-loop-'+owned['token']
 payload=io.BytesIO()
 with tarfile.open(fileobj=payload,mode='w') as archive:
  for name,data in files.items():
   info=tarfile.TarInfo(name);info.size=len(data);info.mode=0o644;archive.addfile(info,io.BytesIO(data))
 plan=("set -e\nawk '$2==\""+usb+"\" && $4 ~ /(^|,)rw(,|$)/ {ok=1} END {exit !ok}' /proc/mounts\n"
       'test -d '+usb+'\ntest -w '+usb+'\ntest ! -e '+target+'\nmkdir -p '+usb+'/MODS/community-mods\nmkdir '+stage+'\ntar -xf - -C '+stage+'\nmv '+stage+' '+target+'\nsync\nsha256sum '+ ' '.join(target+'/'+name for name in files)+'\n')
 staged=command(ssh+[plan],input=payload.getvalue(),timeout=20).decode()
 actual={line.split()[1].rsplit('/',1)[-1]:line.split()[0] for line in staged.splitlines() if len(line.split())==2}
 if actual!={name:hashlib.sha256(data).hexdigest() for name,data in files.items()}:raise ValueError('real USB staged package hashes mismatch')
 if debug:Path(str(output)+'-usb-stage.txt').write_text(staged)
 flags=('azmod.native-exit.on','azmod.native-reloop.on','azmod.native-loop-commands.on')
 extra=command(ssh+['set -e\ntouch '+' '.join('/home/root/settings/'+flag for flag in flags)+'\n'+''.join('test -f /home/root/settings/'+flag+' && echo COMMAND_FLAG '+flag+'\n' for flag in flags)],timeout=10).decode()
 if set(extra.splitlines())!={'COMMAND_FLAG '+flag for flag in flags}:raise ValueError('command startup gates unavailable')
 if debug:Path(str(output)+'-command-flags.txt').write_text(extra)
 adapter_binding=None
 if debug:
  adapter_binding=dict(adapter_sha256=sha(__file__),package_manifest_sha256=sha(Path(package)/'runtime.json'),
   probe_manifest_sha256=sha(probe_manifest),probe_binary_sha256=pb['binary']['sha256'],probe_package_sha256=pb['package']['sha256'],
   owner_nonce=owned['token'],container=owned['container'],command_flags=list(flags),usb_mount=usb)
  Path(str(output)+'-adapter-inputs.json').write_text(json.dumps(adapter_binding,indent=2)+'\n')
 prepare_allow=context['prepare_allow']
 meta={};sessions=[None];read_index=[0];phase=output/'loaded'
 def independent(pid,birth):
  owner();data=command(ssh+[native_birth_check_script(pid,birth)],timeout=5)
  if data not in (b'AZSDK_NATIVE_BIRTH_MATCH\n',b'AZSDK_NATIVE_BIRTH_REFUSED\n'):raise ValueError('native birth check unavailable')
  return data==b'AZSDK_NATIVE_BIRTH_MATCH\n'
 def bounded_read(body,cap=1048576):
  owner();data=command(ssh+[read_script(meta['pid'],meta['birth'],body,cap)],timeout=15)
  if len(data)>cap:raise ValueError('owned read limit')
  return data.decode()
 def loaded_executor(stopped,remaining,verify_new):
  def factory(pid,birth,guard):
   guard();verify_new(pid,birth)
   stage='/tmp/az-sdk-panel.'+uuid.uuid4().hex
   command(ssh+['umask 077; mkdir '+stage+'; cat >'+stage+'/receiver; chmod 700 '+stage+'/receiver'],input=RECEIVER.read_bytes(),timeout=15)
   meta.update(pid=pid,birth=birth,owner=owned['token'],stage=stage)
   (phase/'panel-transport.json').write_text(json.dumps(meta,indent=2)+'\n')
   s=PanelTransportSessions(pid,birth,owned['token'],guard,independent,lambda label:PanelTransport(ssh,stage+'/receiver',sha(RECEIVER),pid,birth,owned['token'],guard),max_sessions=6)
   sessions[0]=s;s.renew('navigation',neutral=True)
   command(ssh+['test -x /home/root/scripts/usb-renotify.sh; AZ_RENOTIFY_LOG='+stage+'/renotify.log /home/root/scripts/usb-renotify.sh --until-ack </dev/null >'+stage+'/renotify.stdout 2>&1 &'],timeout=15)
   return s
  captured={'raw':None,'fresh':False}
  def capture():
   # One independently PID/birth guarded read captures service health, short
   # READY and complete result rows together. No positive identity cache/TTL.
   results=target+'/data/results.'+meta['pid']+'.log'
   body=('cat '+LOG+"; printf '\n__AZSDK_LOOP_RESULTS__\n'; test ! -f "+results+' || cat '+results+
         "; printf '\n__AZSDK_LOOP_HEALTH__\n'; systemctl show az-sdk-runtime.service -p NRestarts -p ActiveState -p SubState")
   data=bounded_read(body)
   core_log,rest=data.split('\n__AZSDK_LOOP_RESULTS__\n',1)
   result_log,service=rest.split('\n__AZSDK_LOOP_HEALTH__\n',1)
   # Newline commits a receipt; concurrent EOF can expose an append fragment.
   # Defer only that unfinished tail, preserving strict complete-row parsing.
   if result_log and not result_log.endswith('\n'):
    result_log=result_log.rsplit('\n',1)[0]+'\n' if '\n' in result_log else ''
   if ready(core_log,meta['pid'])['data']!=target+'/data':raise ValueError('probe data_dir differs from staged USB owner')
   if not all(item in service.splitlines() for item in ('NRestarts=0','ActiveState=active','SubState=running')):
    raise ValueError('owned native service identity/health changed')
   raw=core_log+'\n'+result_log
   read_index[0]+=1
   if debug:
    (phase/('command-read-%04d.txt'%read_index[0])).write_text(raw)
    (phase/('command-health-%04d.txt'%read_index[0])).write_text(service)
   captured.update(raw=raw,fresh=True)
   return raw
  def read_log():
   # LoopClient calls health then read_log immediately. Consume that same
   # bounded capture once, rather than issuing four separate SSH reads.
   if not captured['fresh']:capture()
   captured['fresh']=False
   return captured['raw']
  def health():
   capture()  # native PID/birth checked before and after this actual read
   return True
  def send_request(data):
   directory=target+'/data'
   file=directory+'/.request-'+uuid.uuid4().hex
   script=('set -e\ntest ! -L '+directory+'\nfor i in 1 2 3 4 5 6 7 8 9 10; do test ! -e '+directory+'/request -a ! -e '+directory+'/request.processing && break; sleep .1; done\n'
           'test ! -e '+directory+'/request\ntest ! -e '+directory+'/request.processing\numask 077\ncat >'+file+'\nmv '+file+' '+directory+'/request\n')
   # Guard writes as well as reads; the payload's PID is an additional check
   # performed by the approved probe, not a replacement for native birth.
   guarded=read_script(meta['pid'],meta['birth'],script,1024)
   owner();command(ssh+[guarded],input=data.encode() if isinstance(data,str) else data,timeout=10)
  def action(controller):
   controller._release(controller.driver.BUS_ERP);controller._release(controller.driver.BUS_MIXER)
   path='/home/root/settings/az-usbload/community/'+ID+'/'+ID+'.so'
   maps=bounded_read('cat /proc/'+meta['pid']+'/maps')
   if not any(line.split()[-1:]==[path] for line in maps.splitlines()):raise MissingEvidence('normal loader mapping absent')
   if bounded_read('sha256sum '+path,4096).split()!=[pb['binary']['sha256'],path]:raise ValueError('loaded probe hash mismatch')
   def setup_deck(deck):
    logical=deck+1
    sessions[0].renew('load-deck%d'%logical,neutral=controller.panel_inputs_neutral())
    ensure_quantize_off(controller)
    selection=context['prepare_deck'](controller,logical)
    if not isinstance(selection,dict) or selection.get('status')!='PASS' or selection.get('selected_title') not in titles or not isinstance(selection.get('evidence'),str):raise MissingEvidence('current independently observed test selection required')
    evidence=controller.screenshot('deck%d-selected-track'%logical)
    controller.events.append(dict(action='selection_observed',deck=logical,evidence=evidence))
    saved_timeout=controller.timeout
    try:
     controller.timeout=180
     controller.load(logical)
     snapshot=controller.wait(lambda s:s['decks'][deck]['loaded'],'deck%d loaded'%logical)
     if snapshot['decks'][deck].get('title') not in titles:raise MissingEvidence('exact loaded test title unavailable')
    finally:controller.timeout=saved_timeout
    # One explicit setup PLAY; actual source history must then advance.
    controller.button(logical,'play')
    wait_playback_motion(controller,logical,snapshot)
   refusals=run_refusals(read_log,send_request,health,meta['pid'],sample_rate=binding['engine_rate'])
   if debug:(phase/'admission-refusals.json').write_text(json.dumps(refusals,indent=2)+'\n')
   if refusals.get('status')!='PASS':
    result=dict(status=refusals.get('status','INCOMPLETE'),errors=list(refusals.get('errors',[])),missing=list(refusals.get('missing',[])),decks={},evidence=[],admission_refusals=refusals)
   else:
    result=run_commands(read_log,send_request,health,meta['pid'],setup_deck=setup_deck,sample_rate=binding['engine_rate'])
    result['admission_refusals']=refusals
    if result.get('status')=='PASS':
     try:result['continuation']=run_continuation_after_panel_close(controller,sessions[0],read_log,send_request,health,int(meta['pid']),bounded_read,binding,expected_titles=titles)
     except Exception as error:
      result['status']='FAIL'
      result.setdefault('errors',[]).append(str(error))
   if result.get('status')!='PASS' and callable(context.get('on_failure')):
    context['on_failure'](controller,result,bounded_read)
   result.update(pid=meta['pid'],birth=meta['birth'],mapped_probe_sha256=pb['binary']['sha256'],events=list(controller.events))
   if debug:(phase/'loop-commands.json').write_text(json.dumps(result,indent=2)+'\n')
   return result
  def validate(loop,source,health_rows,identity,manifest,firmware_sha,controller_report,*unused):
   report=dict(controller_report or dict(status='INCOMPLETE',missing=['controller did not produce command report']))
   report['health']=validate_health(health_rows)
   errors=list(report.get('errors',[]));missing=list(report.get('missing',[]))
   if report['health']['status']=='FAIL':errors+=report['health'].get('errors',[])
   elif report['health']['status']!='PASS':missing.append('stable SDK runtime health')
   if len(health_rows)<2 or compare_timestamps(health_rows[-1]['local_monotonic']-health_rows[0]['local_monotonic'],120,units_per_second=1)<0:
    missing.append('120-second stable SDK runtime observation')
   if health_rows and identity.get('pid')!=health_rows[0].get('pid'):errors.append('mapped native process identity mismatch')
   for name,expected in manifest['sha256'].items():
    if identity.get('mapped',{}).get(name)!=expected:errors.append('mapped core/diagnostic hash mismatch '+name)
   if identity.get('firmware_sha256')!=firmware_sha:errors.append('actual firmware hash mismatch')
   report.update(status='FAIL' if errors else 'INCOMPLETE' if missing else report.get('status','INCOMPLETE'),errors=errors,missing=missing)
   return report
  context=dict(owned,driver_path=DRIVER,driver_sha256=owned['driver_sha256'],position_binding=binding,stopped_identity=stopped,expected_titles=titles,panel_transport_factory=factory,verify_process=independent)
  result=run_observed_runtime(ssh,package,source_root,phase,firmware,remaining,command,context,execute_controls=action,validate_observed=validate)
  result.update(pid=meta.get('pid'),birth=meta.get('birth'),mapped_probe_sha256=pb['binary']['sha256'] if meta else None)
  return result
 config=dict(owned,driver_path=DRIVER,driver_sha256=owned['driver_sha256'],receiver_path=RECEIVER,receiver_sha256=owned['receiver_sha256'],position_binding=binding,probe_manifest=probe_manifest,probe_package=probe_package,probe_id=ID,prepare_allow=prepare_allow,run_settings=False,loaded_executor=loaded_executor)
 report=run_public_runtime(ssh,package,source_root,output,firmware,seconds,command,config)
 if debug:
  report['loop_adapter_binding']=adapter_binding
  (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
 return report
