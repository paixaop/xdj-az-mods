#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Persistent, acknowledged panel transport for one owned emulator process.

The caller uploads the digest-bound native sdk_panel_receiver to its owned RAM
directory. This module starts exactly one supplied strict loopback SSH channel;
it never imports azemu, connects a global socket or uploads a binary itself.

``guard()`` must check live Docker ownership and may additionally check process
birth. It returns the expected token or None on success, and raises on failure.
``verify_process`` deliberately does NOT call guard: it is a read-only native
CHECK intended for the process-check portion of that very guard. This prevents
recursive ownership/process verification. Native code rechecks birth before
every device write, including neutral bus clears on close or EOF.
"""
import math
import os
import re
import select
import struct
import subprocess
import threading
import time


class PanelTransport:
    def __init__(self,ssh,receiver,receiver_sha256,pid,birth,owner_token,guard,
                 timeout=5,popen=subprocess.Popen):
        if not isinstance(owner_token,str) or not re.fullmatch('[0-9a-f]{32}',owner_token):raise ValueError('owned emulator token required')
        if not callable(guard):raise ValueError('owned emulator guard required')
        if type(timeout) not in (int,float) or not math.isfinite(timeout) or not 0<timeout<=30:raise ValueError('bounded transport timeout required')
        if not isinstance(pid,str) or not re.fullmatch('[1-9][0-9]{0,9}',pid) or int(pid)>2147483647:
            raise ValueError('one pinned PID required')
        if not isinstance(birth,str) or not re.fullmatch('[1-9][0-9]{0,19}',birth) or int(birth)>=2**64:
            raise ValueError('one pinned process birth required')
        if not isinstance(receiver,str) or not re.fullmatch('/tmp/az-sdk-panel\\.[0-9a-f]{32}/receiver',receiver):raise ValueError('owned native receiver path required')
        if not isinstance(receiver_sha256,str) or not re.fullmatch('[0-9a-f]{64}',receiver_sha256):raise ValueError('native receiver SHA256 required')
        self._strict_ssh(ssh)
        owned_ssh=list(ssh)
        if '-F' not in owned_ssh:owned_ssh[1:1]=['-F','/dev/null']
        if '-T' not in owned_ssh:owned_ssh.insert(1,'-T')
        self.owner_token=owner_token;self.guard=guard;self.pid=pid;self.birth=birth;self.timeout=timeout
        self.sequence=0;self.closed=False;self.process=None;self.buffer=b'';self.lock=threading.RLock()
        self._owner()
        # All interpolated values have restrictive token validation above.
        remote='set -e; actual=$(sha256sum '+receiver+'); test "${actual%% *}" = '+receiver_sha256+'; exec '+receiver+' '+pid+' '+birth
        try:
            self.process=popen(owned_ssh+[remote],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,bufsize=0)
            os.set_blocking(self.process.stdin.fileno(),False);os.set_blocking(self.process.stdout.fileno(),False)
            ready=self._line(time.monotonic()+self.timeout)
            if ready!='READY 1 '+pid+' '+birth:raise RuntimeError('native receiver identity/handshake mismatch')
        except BaseException:
            self._abort();raise

    @staticmethod
    def _strict_ssh(ssh):
        if not isinstance(ssh,(list,tuple)) or not ssh or ssh[0]!='ssh' or ssh[-1]!='root@127.0.0.1' or any(not isinstance(v,str) or '\0' in v for v in ssh):
            raise ValueError('strict owned loopback SSH required')
        options={};seen=set();index=1
        allowed={'IdentitiesOnly','BatchMode','LogLevel','StrictHostKeyChecking','UserKnownHostsFile',
                 'ConnectTimeout','ServerAliveInterval','ServerAliveCountMax'}
        while index<len(ssh)-1:
            flag=ssh[index];index+=1
            if flag!='-o':
                if flag in seen:raise ValueError('duplicate SSH transport option')
                seen.add(flag)
            if flag=='-T':continue
            if flag not in ('-o','-i','-p','-F') or index>=len(ssh)-1:raise ValueError('unsupported SSH transport option')
            value=ssh[index];index+=1
            if flag=='-o':
                key,sep,setting=value.partition('=')
                if not sep or key in options or key not in allowed:raise ValueError('unambiguous strict SSH options required')
                options[key]=setting
            elif flag=='-p' and (not value.isdigit() or not 1<=int(value)<=65535):raise ValueError('invalid owned SSH port')
            elif flag=='-F' and value!='/dev/null':raise ValueError('SSH configuration override forbidden')
        if '-p' not in seen:raise ValueError('explicit owned SSH port required')
        known=options.get('UserKnownHostsFile','')
        if options.get('StrictHostKeyChecking')!='yes' or options.get('BatchMode')!='yes' or not re.fullmatch(r'/[^\s]+',known) or known=='/dev/null':
            raise ValueError('strict pinned SSH host identity required')
        if not options.get('ConnectTimeout','').isdigit() or not 1<=int(options['ConnectTimeout'])<=30:
            raise ValueError('bounded SSH connection timeout required')
        if 'IdentitiesOnly' in options and options['IdentitiesOnly']!='yes':raise ValueError('explicit SSH identity required')
        if 'LogLevel' in options and options['LogLevel'] not in ('ERROR','FATAL','QUIET'):raise ValueError('bounded SSH diagnostics required')
        for key,maximum in [('ServerAliveInterval',30),('ServerAliveCountMax',10)]:
            if key in options and (not options[key].isdigit() or not 1<=int(options[key])<=maximum):raise ValueError('bounded SSH keepalive required')

    def _owner(self):
        value=self.guard()
        if value is not None and value!=self.owner_token:raise ValueError('emulator ownership changed')

    def _wait(self,fd,write,deadline):
        while True:
            remaining=deadline-time.monotonic()
            if remaining<=0:raise TimeoutError('panel transport deadline expired')
            try:
                readable,writable,_=select.select([] if write else [fd],[fd] if write else [],[],remaining)
            except InterruptedError:continue
            if readable or writable:return

    def _line(self,deadline):
        while b'\n' not in self.buffer:
            if len(self.buffer)>256:raise RuntimeError('native receiver acknowledgement exceeds limit')
            self._wait(self.process.stdout.fileno(),False,deadline)
            try:data=os.read(self.process.stdout.fileno(),257-len(self.buffer))
            except BlockingIOError:continue
            if not data:raise RuntimeError('native receiver closed channel')
            self.buffer+=data
        line,self.buffer=self.buffer.split(b'\n',1)
        if len(line)>256:raise RuntimeError('native receiver acknowledgement exceeds limit')
        try:return line.decode('ascii')
        except UnicodeDecodeError:raise RuntimeError('invalid native receiver acknowledgement')

    def _request(self,action,payload=b'',check_owner=True):
        try:
            if self.closed:raise RuntimeError('panel transport is closed')
            if check_owner:self._owner()
            with self.lock:
                if self.closed:raise RuntimeError('panel transport is closed')
                self.sequence+=1
                if self.sequence>=2**64:raise RuntimeError('panel transport sequence exhausted')
                request=action+struct.pack('!Q',self.sequence)+payload
                deadline=time.monotonic()+self.timeout;offset=0
                while offset<len(request):
                    self._wait(self.process.stdin.fileno(),True,deadline)
                    try:count=os.write(self.process.stdin.fileno(),request[offset:])
                    except BlockingIOError:continue
                    if count<=0:raise RuntimeError('native receiver input closed')
                    offset+=count
                reply=self._line(deadline)
                if reply.startswith('ERR '+str(self.sequence)+' '):raise RuntimeError('native receiver '+reply.split(' ',2)[2])
                if reply!='OK '+str(self.sequence):raise RuntimeError('native receiver acknowledgement mismatch')
            return True
        except BaseException:
            self._abort();raise

    def send(self,payload):
        if type(payload) is not bytes or len(payload)!=153 or payload[0]>3:
            raise ValueError('one complete 153-byte native panel frame required')
        return self._request(b'F',payload)

    def verify_process(self,pid,birth):
        """Read-only native birth check; caller must perform ownership checking.

        Guard is intentionally not invoked here, so a guard can call this method
        without recursing. No device writes occur for the CHECK operation.
        """
        if pid!=self.pid or birth!=self.birth:return False
        try:return self._request(b'C',check_owner=False)
        except RuntimeError:return False

    def _abort(self):
        self.closed=True
        if self.process is None:return
        try:self.process.stdin.close()
        except (OSError,ValueError):pass
        try:self.process.wait(timeout=1)
        except subprocess.TimeoutExpired:
            self.process.kill()
            try:self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:pass
        finally:
            try:self.process.stdout.close()
            except (OSError,ValueError):pass

    def close(self):
        if self.closed:return
        try:self._request(b'Q')
        finally:self._abort()


class PanelTransportSessions:
    """Finite receiver leases renewed only at an explicit neutral boundary.

    Each factory call creates a fresh receiver with the same pinned identity.
    Native receiver limits and the caller's overall deadline remain unchanged.
    CHECK failure falls back to an independent read-only birth check; a failed
    stateful write permanently disables sends and renewal for this session.
    """
    def __init__(self,pid,birth,owner_token,guard,check_process,factory,max_sessions=6):
        if not isinstance(pid,str) or not re.fullmatch('[1-9][0-9]{0,9}',pid) or int(pid)>2147483647:
            raise ValueError('one pinned PID required')
        if not isinstance(birth,str) or not re.fullmatch('[1-9][0-9]{0,19}',birth) or int(birth)>=2**64:
            raise ValueError('one pinned process birth required')
        if not isinstance(owner_token,str) or not re.fullmatch('[0-9a-f]{32}',owner_token):raise ValueError('owned emulator token required')
        if not all(callable(f) for f in (guard,check_process,factory)):raise ValueError('owned receiver factory and independent birth check required')
        if type(max_sessions) is not int or not 1<=max_sessions<=6:raise ValueError('finite receiver session limit required')
        self.pid=pid;self.birth=birth;self.owner_token=owner_token;self.guard=guard
        self.check_process=check_process;self.factory=factory;self.max_sessions=max_sessions
        self.active=None;self.sessions=[];self.closed=False;self.failed=False;self.renewing=False

    def _owner(self):
        value=self.guard()
        if value is not None and value!=self.owner_token:raise ValueError('emulator ownership changed')

    def _native(self):
        # Exceptions mean the independent check was unavailable, not evidence
        # that the process changed. The caller records that transport failure.
        return self.check_process(self.pid,self.birth) is True

    def _identity(self,value):
        if (getattr(value,'pid',None),getattr(value,'birth',None),getattr(value,'owner_token',None))!=(self.pid,self.birth,self.owner_token):
            raise ValueError('receiver lease identity mismatch')

    def renew(self,label,neutral=False):
        if self.failed:raise RuntimeError('panel session failed; renewal/retry forbidden')
        if self.closed:raise RuntimeError('panel session is closed')
        if type(neutral) is not bool or not neutral:raise ValueError('acknowledged neutral panel boundary required')
        if not isinstance(label,str) or not re.fullmatch('[a-z][a-z0-9-]{0,63}',label):raise ValueError('bounded receiver phase label required')
        if len(self.sessions)>=self.max_sessions:raise RuntimeError('receiver session limit reached')
        self.renewing=True
        try:
            self._owner()
            if not self._native():raise ValueError('pinned native process changed before receiver renewal')
            if self.active is not None:
                try:self.active.close()
                except (RuntimeError,OSError,TimeoutError):
                    if not self.active.closed:raise
                self.active=None
            if not self._native():raise ValueError('pinned native process changed during receiver renewal')
            value=None
            try:
                value=self.factory(label)
                self._identity(value)
                if value.closed or type(value.sequence) is not int or value.sequence!=0:
                    raise ValueError('fresh live receiver with sequence zero required')
            except BaseException:
                self.failed=True
                if value is not None:
                    try:value.close()
                    except (RuntimeError,OSError,TimeoutError):pass
                raise
            self.active=value
            self.sessions.append({'phase':label,'pid':self.pid,'birth':self.birth,'initial_sequence':0})
        finally:self.renewing=False

    def send(self,payload):
        if self.failed:raise RuntimeError('panel session failed; stateful retry forbidden')
        if self.closed or self.active is None:raise RuntimeError('panel session has no live receiver')
        if type(payload) is not bytes or len(payload)!=153 or payload[0]>3:raise ValueError('one complete 153-byte native panel frame required')
        try:
            self._owner();self._identity(self.active)
            return self.active.send(payload)
        except BaseException:
            self.failed=True;raise

    def verify_process(self,pid,birth):
        """Read-only verification; ownership must be checked by the caller."""
        if (pid,birth)!=(self.pid,self.birth):return False
        if self.renewing or self.closed or self.active is None or self.active.closed:return self._native()
        try:
            self._identity(self.active)
            if self.active.verify_process(pid,birth):return True
        except (RuntimeError,OSError,TimeoutError):pass
        return self._native()

    def close(self):
        if self.closed:return
        self.closed=True
        if self.active is not None:self.active.close()
