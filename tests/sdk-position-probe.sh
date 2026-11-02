#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only
# Build one exact-ABI read-only preload witness without contacting any target.
set -euo pipefail
if [[ $# != 2 ]]; then
    echo "Usage: $0 NEW_OUTPUT_DIRECTORY AZSTEM_SO" >&2
    exit 2
fi
script_directory=$(cd -- "$(dirname -- "$0")" && pwd)
python3 - "$script_directory/.." "$1" "$2" <<'PY'
import hashlib,json,os,re,subprocess,sys
from pathlib import Path
root,out,provider=[Path(p).resolve() for p in sys.argv[1:]]
if out.exists() or not out.parent.is_dir() or not provider.is_file(): raise SystemExit('new output directory and existing azstem.so required')
image=os.environ.get('BUILD_IMAGE','azmod-build:18.04')
if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9._:/@-]{0,255}',image): raise SystemExit('invalid build image')
def command(argv):
    p=subprocess.run(argv,stdout=subprocess.PIPE,stderr=subprocess.PIPE,universal_newlines=True,timeout=180)
    if p.returncode: raise RuntimeError(p.stderr[-4000:])
    return p.stdout
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
provider_hash=digest(provider)
image_id=command(['docker','image','inspect','--format','{{.Id}}',image]).strip()
architecture=command(['docker','image','inspect','--format','{{.Architecture}}',image_id]).strip()
if architecture!='arm64': raise SystemExit('the existing build image must be arm64')
readelf=command(['docker','run','--rm','--platform','linux/arm64','--entrypoint','/usr/bin/readelf','-v',str(provider.parent)+':/provider:ro',image_id,'-n','/provider/'+provider.name])
ids=re.findall(r'Build ID:\s*([0-9a-f]+)',readelf)
if len(ids)!=1 or not re.fullmatch('[0-9a-f]{2,128}',ids[0]) or len(ids[0])%2: raise SystemExit('one bounded GNU build ID required')
out.mkdir()
sources=['tests/integration/sdk_position_probe.c','src/common/azdeckinfo.h','src/common/azliveobserver.h','src/azstem/ep147rate.h','src/azstem/sdk_export.c']
frozen=out/'source'
source_hashes={}
for name in sources:
    data=(root/name).read_bytes(); target=frozen/name; target.parent.mkdir(parents=True,exist_ok=True); target.write_bytes(data)
    source_hashes[name]=hashlib.sha256(data).hexdigest()
header_hash=source_hashes['src/common/azdeckinfo.h']
flags=['-std=gnu11','-O2','-g','-fPIC','-shared','-pthread','-Wall','-Wextra','-Werror','-Wl,-z,relro,-z,now',
       '-DAZSDK_POSITION_HEADER_SHA256="'+header_hash+'"',
       '-DAZSDK_POSITION_PROVIDER_SHA256="'+provider_hash+'"',
       '-DAZSDK_POSITION_BUILD_ID="'+ids[0]+'"']
argv=['docker','run','--rm','--platform','linux/arm64','--entrypoint','/usr/bin/gcc','-v',str(frozen)+':/src:ro','-v',str(out)+':/out',image_id]+flags+['/src/tests/integration/sdk_position_probe.c','-ldl','-lm','-o','/out/sdk-position-probe.so']
command(argv)
if digest(provider)!=provider_hash: raise SystemExit('provider changed during build')
if any(digest(frozen/name)!=value or digest(root/name)!=value for name,value in source_hashes.items()): raise SystemExit('source changed during build')
toolchain=command(['docker','run','--rm','--platform','linux/arm64','--entrypoint','/usr/bin/gcc',image_id,'--version'])
manifest={'schema':1,'scope':'copied engine-position witness; no native transport-flag semantics or source-file sample-rate claims',
          'build_image':image,'build_image_id':image_id,'toolchain':toolchain,'provider_sha256':provider_hash,'provider_build_id':ids[0],
          'header_sha256':header_hash,'source_hashes':source_hashes,'binary_sha256':digest(out/'sdk-position-probe.so'),
          'path':'/tmp/az-sdk-position.PID.json','stop_marker':'/tmp/az-sdk-position.PID.stop (owned regular0600)',
          'opt_in':'AZSDK_POSITION_PROBE=1; /proc/self/exe basename EP147', 'history_samples':12,'period_ms':100,'window_seconds':1800}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print(json.dumps({'binary':str(out/'sdk-position-probe.so'),'sha256':manifest['binary_sha256'],'provider_build_id':ids[0]},indent=2))
PY
