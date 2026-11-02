#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Portable transport contract tests; no VM, network, device or deck access."""
import importlib.util
import json
import os
from pathlib import Path
import select
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[2]
SPEC=importlib.util.spec_from_file_location('panel_transport',str(ROOT/'tests/integration/sdk_panel_transport.py'))
panel=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(panel)
TOKEN='a'*32
SSH=['ssh','-T','-o','StrictHostKeyChecking=yes','-o','UserKnownHostsFile=/tmp/owned-known-hosts',
     '-o','BatchMode=yes','-o','ConnectTimeout=5','-p','22222','root@127.0.0.1']
REMOTE='/tmp/az-sdk-panel.'+TOKEN+'/receiver'

# The local fixture speaks the native receiver's fixed-length request protocol.
# Each test's injected Popen ignores the SSH argv; network access is impossible.
SERVER=r'''
import os,struct,sys,time
mode=sys.argv[1]
def exact(n):
 result=b''
 while len(result)<n:
  chunk=os.read(0,n-len(result))
  if not chunk:return None
  result+=chunk
 return result
if mode=='no-ready':time.sleep(2);sys.exit()
if mode=='no-ack':time.sleep(.075) # Deliberate cold-peer startup before a silent request.
os.write(1,b'READY 1 705 12345\n')
while True:
 h=exact(9)
 if h is None:break
 action=h[:1];sequence=struct.unpack('!Q',h[1:])[0]
 payload=exact(153) if action==b'F' else None
 if mode=='no-ack':time.sleep(2);break
 if mode=='bad-ack':os.write(1,b'OK 999\n');continue
 if mode=='changed':os.write(1,('ERR %d process\n'%sequence).encode());break
 os.write(1,('OK %d\n'%sequence).encode())
 if action==b'Q':break
'''

class TransportTests(unittest.TestCase):
    def setUp(self):
        self.tmp=tempfile.TemporaryDirectory();self.addCleanup(self.tmp.cleanup)
        self.server=Path(self.tmp.name)/'server.py';self.server.write_text(SERVER)
        self.mode='ok';self.calls=[];self.owner=TOKEN
    def launch(self,argv,**options):
        self.calls.append(argv)
        return subprocess.Popen([sys.executable,str(self.server),self.mode],**options)
    def transport(self,**options):
        args=dict(ssh=SSH,receiver=REMOTE,receiver_sha256='b'*64,pid='705',birth='12345',
                  owner_token=TOKEN,guard=lambda:self.owner,timeout=.2,popen=self.launch)
        args.update(options)
        t=panel.PanelTransport(**args);self.addCleanup(t.close);return t
    def test_one_persistent_channel_serves_frames_and_birth_checks(self):
        t=self.transport()
        for bus in range(4):self.assertTrue(t.send(bytes([bus])+bytes(range(152))))
        self.assertTrue(t.verify_process('705','12345'))
        self.assertFalse(t.verify_process('706','12345'))
        self.assertFalse(t.verify_process('705','12346'))
        t.close();t.close()
        self.assertEqual(len(self.calls),1)
        self.assertEqual(self.calls[0][:-1],SSH[:1]+['-F','/dev/null']+SSH[1:])
        self.assertIn('sha256sum',self.calls[0][-1])
    def test_bad_frames_fail_before_any_request(self):
        t=self.transport();sequence=t.sequence
        for payload in [b'',b'\0'*152,b'\0'*154,b'\4'+b'\0'*152,'x'*153,bytearray(153)]:
            with self.subTest(payload=type(payload).__name__),self.assertRaises(ValueError):t.send(payload)
        self.assertEqual(t.sequence,sequence)
        self.assertTrue(t.verify_process('705','12345'))
    def test_owner_change_fails_before_frame(self):
        t=self.transport();sequence=t.sequence;self.owner='c'*32
        with self.assertRaisesRegex(ValueError,'ownership'):t.send(b'\0'*153)
        self.assertEqual(t.sequence,sequence)
    def test_wrong_owner_starts_no_process(self):
        self.owner='c'*32
        with self.assertRaisesRegex(ValueError,'ownership'):self.transport()
        self.assertEqual(self.calls,[])
    def test_remote_birth_change_fails_closed(self):
        self.mode='changed';t=self.transport()
        with self.assertRaisesRegex(RuntimeError,'process'):t.send(b'\0'*153)
        with self.assertRaisesRegex(RuntimeError,'closed'):t.send(b'\0'*153)
    def test_mismatched_ack_fails_closed(self):
        self.mode='bad-ack';t=self.transport()
        with self.assertRaisesRegex(RuntimeError,'acknowledgement'):t.send(b'\0'*153)
    def test_silent_peer_has_bounded_start_and_request_deadlines(self):
        self.mode='no-ready'
        with self.assertRaisesRegex(TimeoutError,'deadline'):self.transport(timeout=.05)
        # Startup and the silent request are independent finite phases.
        self.mode='no-ack';t=self.transport(timeout=.5);self.assertFalse(t.closed)
        t.timeout=.05
        with self.assertRaisesRegex(TimeoutError,'deadline'):t.send(b'\0'*153)
        self.assertTrue(t.closed)
    def test_strict_owned_loopback_and_remote_binary_identity_are_required(self):
        for changes in [dict(ssh=['ssh','root@example.com']),dict(ssh=['ssh','root@127.0.0.1']),
                        dict(ssh=SSH[:-1]+['root@example.com']),dict(receiver='/tmp/receiver; touch /tmp/oops'),
                        dict(receiver_sha256='x'),dict(pid='0'),dict(pid='705;true'),dict(birth='0'),
                        dict(owner_token='x'),dict(timeout=0),dict(timeout=float('nan'))]:
            with self.subTest(changes=changes),self.assertRaises(ValueError):self.transport(**changes)
        self.assertEqual(self.calls,[])
    def test_process_guard_can_call_read_only_verification_without_recursion(self):
        holder={};calls=[]
        def guard():
            calls.append(True)
            if holder:self.assertTrue(holder['transport'].verify_process('705','12345'))
        t=self.transport(guard=guard);holder['transport']=t
        self.assertTrue(t.send(b'\0'*153))
        count=len(calls)
        self.assertTrue(t.verify_process('705','12345'))
        self.assertEqual(len(calls),count)
        t.close()
    def test_ssh_endpoint_overrides_and_ambiguous_key_or_port_are_rejected(self):
        for extra in [['-o','ProxyCommand=nc attacker 22'],['-o','HostName=attacker'],['-o','RemoteCommand=sh'],
                      ['-o','PermitLocalCommand=yes'],['-o','LocalCommand=touch /tmp/oops'],['-o','ControlPath=/tmp/shared'],
                      ['-p','22223'],['-i','/tmp/key-a','-i','/tmp/key-b'],['-o','StrictHostKeyChecking=no'],
                      ['-o','ServerAliveInterval=0'],['-o','ServerAliveCountMax=999999']]:
            with self.subTest(extra=extra),self.assertRaises(ValueError):self.transport(ssh=SSH[:-1]+extra+SSH[-1:])
        self.assertEqual(self.calls,[])


class LeaseTests(unittest.TestCase):
    """Pure Python phase leases; no compiler, target or real sleep."""
    def setUp(self):
        self.now=0;self.alive=True;self.owner=TOKEN;self.order=[];self.receivers=[]
        case=self
        class Receiver:
            pid='705';birth='12345';owner_token=TOKEN
            def __init__(self):self.closed=False;self.sent=[];self.end=case.now+900;self.sequence=0
            def verify_process(self,pid,birth):
                if self.closed or case.now>=self.end:
                    self.closed=True;raise RuntimeError('receiver expired')
                return case.alive and pid==self.pid and birth==self.birth
            def send(self,payload):
                if not self.verify_process(self.pid,self.birth):raise RuntimeError('native process changed')
                self.sent.append(payload)
                if getattr(self,'fail_write',False):raise BrokenPipeError('ambiguous write')
                return True
            def close(self):self.closed=True;case.order.append('close')
        def factory(label):
            self.order.append('start:'+label);value=Receiver();self.receivers.append(value);return value
        self.session=panel.PanelTransportSessions('705','12345',TOKEN,lambda:self.owner,
            lambda pid,birth:self.alive and pid=='705' and birth=='12345',factory,max_sessions=3)
        self.addCleanup(self.session.close)
    def test_expired_check_does_not_claim_native_death_and_neutral_phase_renewal_never_replays(self):
        self.session.renew('navigation',neutral=True)
        first=b'\0'*153;second=b'\2'+b'\0'*152
        self.session.send(first);self.now=901
        self.assertTrue(self.session.verify_process('705','12345'))
        self.session.renew('verification',neutral=True)
        self.session.send(second)
        self.assertEqual([r.sent for r in self.receivers],[[first],[second]])
        self.assertEqual(self.order,['start:navigation','close','start:verification'])
    def test_renewal_requires_neutral_state_and_finite_session_budget(self):
        self.session.renew('first',neutral=True)
        with self.assertRaises(ValueError):self.session.renew('held',neutral=False)
        self.assertEqual(len(self.receivers),1);self.assertFalse(self.receivers[0].closed)
        self.session.renew('second',neutral=True);self.session.renew('third',neutral=True)
        with self.assertRaisesRegex(RuntimeError,'limit'):self.session.renew('fourth',neutral=True)
        self.assertEqual(len(self.receivers),3)
    def test_failed_stateful_write_is_never_replayed_or_renewed(self):
        self.session.renew('first',neutral=True);self.receivers[0].fail_write=True
        frame=b'\0'*153
        with self.assertRaises(BrokenPipeError):self.session.send(frame)
        with self.assertRaisesRegex(RuntimeError,'failed'):self.session.renew('retry',neutral=True)
        with self.assertRaisesRegex(RuntimeError,'failed'):self.session.send(frame)
        self.assertEqual([r.sent for r in self.receivers],[[frame]])
    def test_actual_native_death_or_owner_change_refuses_new_session_before_factory(self):
        self.session.renew('first',neutral=True);self.now=901;self.alive=False
        self.assertFalse(self.session.verify_process('705','12345'))
        with self.assertRaisesRegex(ValueError,'process'):self.session.renew('second',neutral=True)
        self.assertEqual(len(self.receivers),1)
        self.alive=True;self.owner='b'*32
        with self.assertRaisesRegex(ValueError,'ownership'):self.session.renew('second',neutral=True)
        self.assertEqual(len(self.receivers),1)
    def test_renewal_rejects_a_preused_receiver_without_leaking_it_or_allowing_a_second_factory(self):
        original=self.session.factory
        def preused(label):
            value=original(label);value.sequence=1;return value
        self.session.factory=preused
        with self.assertRaisesRegex(ValueError,'fresh'):self.session.renew('first',neutral=True)
        self.assertTrue(self.receivers[0].closed)
        with self.assertRaisesRegex(RuntimeError,'failed'):self.session.renew('retry',neutral=True)
        self.assertEqual(len(self.receivers),1)


class ReceiverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory();cls.folder=Path(cls.tmp.name)
        cls.device=cls.folder/'device';cls.proc=cls.folder/'proc';(cls.proc/'705').mkdir(parents=True)
        cls.binary=cls.folder/'receiver';cls.writes=cls.folder/'writes'
        observed=cls.folder/'observed.c'
        observed.write_text('''#define _POSIX_C_SOURCE 200809L
#define _DARWIN_C_SOURCE
#include <unistd.h>
static ssize_t observed_write(int,const void *,size_t);
#define write observed_write
#include '''+json.dumps(str(ROOT/'tests/integration/sdk_panel_receiver.c'))+'''
#undef write
static ssize_t observed_write(int fd,const void *data,size_t size) {
 if(fd==device_fd) {
  int log=open(SDK_PANEL_LOG,O_WRONLY|O_CREAT|O_APPEND,0600);
  if(log<0)return -1;
  char text[64];int n=snprintf(text,sizeof text,"%zu\\n",size);ssize_t logged=write(log,text,(size_t)n);close(log);
  if(logged!=n)return -1;
  if(getenv("SDK_PANEL_TEST_SHORT_WRITE") && size==153)return write(fd,data,size-1);
 }
 return write(fd,data,size);
}
''')
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2','-DSDK_PANEL_ALLOW_REGULAR=1',
                        '-DSDK_PANEL_PROC_ROOT="'+str(cls.proc)+'"','-DSDK_PANEL_DEVICE="'+str(cls.device)+'"',
                        '-DSDK_PANEL_LOG="'+str(cls.writes)+'"','-o',str(cls.binary),str(observed)],check=True)
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def setUp(self):
        self.device.write_bytes(b'');self.writes.write_text('');self.stat('12345');self.processes=[]
        self.addCleanup(self.cleanup)
    def stat(self,birth,state='S',pid='705'):
        # The command field may itself contain spaces and closing parentheses.
        (self.proc/'705/stat').write_text(pid+' (EP147 strange ) command) '+state+' '+' '.join(['0']*18+[birth])+'\n')
    def launch(self,env=None):
        p=subprocess.Popen([str(self.binary),'705','12345'],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,bufsize=0,env=env)
        self.processes.append(p);return p
    def cleanup(self):
        for p in self.processes:
            if p.poll() is None:
                p.stdin.close()
                try:p.wait(timeout=3)
                except subprocess.TimeoutExpired:p.kill();p.wait()
            for stream in [p.stdin,p.stdout,p.stderr]:
                if not stream.closed:stream.close()
    def line(self,p):
        data=b''
        while not data.endswith(b'\n'):
            self.assertTrue(select.select([p.stdout],[],[],3)[0],'native response deadline')
            part=os.read(p.stdout.fileno(),1)
            self.assertTrue(part,'native response EOF');data+=part
            self.assertLess(len(data),256)
        return data
    def request(self,p,action,sequence,payload=b''):
        p.stdin.write(action+struct.pack('!Q',sequence)+payload)
        return self.line(p)
    def test_fragmented_frames_and_merged_input_keep_exact_device_boundaries(self):
        p=self.launch();self.assertEqual(self.line(p),b'READY 1 705 12345\n')
        frame0=bytes([0])+bytes(range(152));frame2=bytes([2])+b'\xff'*152
        request=b'F'+struct.pack('!Q',1)+frame0
        for byte in request:p.stdin.write(bytes([byte]))
        self.assertEqual(self.line(p),b'OK 1\n')
        # Two requests may arrive together on the pipe; each needs its own
        # native write and acknowledgement. CHECK contributes no device bytes.
        p.stdin.write(b'F'+struct.pack('!Q',2)+frame2+b'C'+struct.pack('!Q',3))
        self.assertEqual(self.line(p),b'OK 2\n');self.assertEqual(self.line(p),b'OK 3\n')
        self.assertEqual(self.device.read_bytes(),frame0+frame2)
        self.assertEqual(self.request(p,b'Q',4),b'OK 4\n');self.assertEqual(p.wait(timeout=3),0)
        self.assertEqual(self.device.read_bytes(),frame0+frame2+b'\x00\x02')
        self.assertEqual(self.writes.read_text().splitlines(),['153','153','1','1'])
    def test_eof_clears_only_used_bus(self):
        p=self.launch();self.line(p);frame=b'\x03'+b'\0'*152
        self.assertEqual(self.request(p,b'F',1,frame),b'OK 1\n')
        p.stdin.close();self.assertEqual(p.wait(timeout=3),0)
        self.assertEqual(self.device.read_bytes(),frame+b'\x03')
    def test_changed_birth_never_writes_or_clears_replacement_process(self):
        p=self.launch();self.line(p);frame=b'\0'*153
        self.assertEqual(self.request(p,b'F',1,frame),b'OK 1\n');self.stat('12346')
        self.assertEqual(self.request(p,b'F',2,frame),b'ERR 2 process\n')
        self.assertEqual(p.wait(timeout=3),1);self.assertEqual(self.device.read_bytes(),frame)
    def test_bad_identity_zombie_and_symlink_refuse_before_device_write(self):
        for mode in ['birth','zombie','pid','symlink']:
            self.stat('12346' if mode=='birth' else '12345',state='Z' if mode=='zombie' else 'S',pid='706' if mode=='pid' else '705')
            if mode=='symlink':
                path=self.proc/'705/stat';path.unlink();path.symlink_to(self.folder/'missing')
            with self.subTest(mode=mode):
                p=self.launch();self.assertEqual(self.line(p),b'ERR 0 process\n');self.assertEqual(p.wait(timeout=3),1)
                self.assertEqual(self.device.read_bytes(),b'')
            if mode=='symlink':(self.proc/'705/stat').unlink()
    def test_replay_unknown_action_and_invalid_bus_write_no_frame(self):
        for action,seq,payload,expected in [(b'C',0,b'',b'ERR 0 sequence\n'),(b'X',1,b'',b'ERR 1 action\n'),
                                                (b'F',1,b'\x04'+b'\0'*152,b'ERR 1 frame\n')]:
            with self.subTest(action=action):
                p=self.launch();self.line(p);self.assertEqual(self.request(p,action,seq,payload),expected)
                self.assertEqual(p.wait(timeout=3),1);self.assertEqual(self.device.read_bytes(),b'')
    def test_partial_frame_eof_cannot_inject_an_incomplete_device_write(self):
        p=self.launch();self.line(p)
        p.stdin.write(b'F'+struct.pack('!Q',1)+b'\0'*152);p.stdin.close()
        self.assertEqual(self.line(p),b'ERR 1 frame\n');self.assertEqual(p.wait(timeout=3),1)
        self.assertEqual(self.device.read_bytes(),b'')
    def test_short_device_write_fails_without_appending_a_frame_remainder(self):
        env=dict(os.environ,SDK_PANEL_TEST_SHORT_WRITE='1');p=self.launch(env=env);self.line(p)
        self.assertEqual(self.request(p,b'F',1,b'\0'*153),b'ERR 1 write\n');self.assertEqual(p.wait(timeout=3),1)
        self.assertEqual(self.writes.read_text().splitlines(),['153','1'])

if __name__=='__main__':unittest.main()
