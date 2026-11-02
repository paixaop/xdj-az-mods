# SPDX-License-Identifier: GPL-3.0-only
"""Extract production private observation functions for asset-independent ABI tests."""
from pathlib import Path
import sys
root=Path(__file__).resolve().parents[2]
out=Path(sys.argv[1]);out.mkdir(parents=True,exist_ok=True)
def fn(s,name):
    # All selected production functions close their outer brace at column zero.
    import re
    m=re.search(r'^'+name+r'[^\n]*\{\n',s,re.M);assert m,name
    b=s.index('\n}',m.start())+2
    return s[m.start():b]+'\n'
s=(root/'src/azstem/deck_track_map.c').read_text()
a=s.index('#define LTI_STR ');b=s.index('static unsigned lti_wlock',a)
# Unused telemetry is not part of the observation fixture.
snap=s[a:b].replace('static _Atomic unsigned g_lti_updates, g_lti_orphans; /* telemetry: copied / no deck matched */\n','')
obs=snap+fn(s,r'static void lti_snap_get\(')+fn(s,r'static int lti_id_zero\(')
s=(root/'src/azstem/sdk_export.c').read_text()
obs+=fn(s,r'int azstem_deck_observation_v1\(')
(out/'observation.inc').write_text(obs)
for name,old,new in [('observation-size','azdeck_record_size(out) < sizeof *out','0'),('observation-padding','memset(id + 12, 0, 4);',';')]:
    assert obs.count(old)==1
    (out/(name+'.inc')).write_text(obs.replace(old,new))
exports=fn(s,r'int azstem_loop_observation_v1\(')+fn(s,r'int azstem_loop_observation_v2\(')
(out/'loop-export.inc').write_text(exports)
for version in (1,2):
    name='azstem_loop_observation_v'+str(version)
    start=exports.index('int '+name)
    i=exports.index('azdeck_record_size(out) < sizeof *out',start)
    broken=exports[:i]+exports[i:].replace('azdeck_record_size(out) < sizeof *out','0',1)
    (out/('loop-export-v'+str(version)+'-size.inc')).write_text(broken)
