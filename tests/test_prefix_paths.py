#!/usr/bin/env python3
"""Explicit-prefix safety without changing HOME or touching default user paths."""
import hashlib,json,os,shutil,subprocess,sys,tempfile
from pathlib import Path
assert os.geteuid()!=0
source=Path(__file__).resolve().parent.parent/'packaging/user'
root=Path(tempfile.mkdtemp(prefix='devstats-prefix-qa-'))
checks=[]
def run(script,*args,ok=True):
 env={k:v for k,v in os.environ.items() if not k.startswith(('DEVSTATS_','XDG_'))}
 assert env.get('HOME')==os.environ.get('HOME')
 result=subprocess.run(['bash',str(script),*map(str,args)],env=env,text=True,capture_output=True,timeout=30)
 assert (result.returncode==0)==ok,(args,result.returncode,result.stdout,result.stderr)
 return result
def bundle(version):
 p=root/('bundle-'+version);p.mkdir()
 for name in ['libexec','share']:(p/name).mkdir()
 files={'.devstats-user-package':'DevStats private Qt runtime v1\n','VERSION':version+'\n','devstats':'#!/bin/sh\nexit 0\n','libexec/devstats':'fixture ELF placeholder; never executed\n','share/devstats.svg':'<svg/>\n'}
 for name in ['install-user.sh','uninstall-user.sh']:files[name]=(source/name).read_text()
 for name,value in files.items():(p/name).write_text(value)
 (p/'devmon').symlink_to('devstats')
 (p/'SHA256SUMS').write_text(''.join(hashlib.sha256((p/name).read_bytes()).hexdigest()+'  '+name+'\n' for name in sorted(files)))
 return p
packages=[bundle(v) for v in ['0.7.9','0.7.10','0.7.11']];prefix=root/'prefix 用户';outside=root/'prefix 用户-other';outside.mkdir();(outside/'devstats').write_text('unrelated external data')
for p in packages:run(p/'install-user.sh','--prefix',prefix)
retained={prefix/'config/devstats/settings.ini':b'[settings]\ncustom=keep\n',prefix/'share/devstats/History/user.txt':'历史'.encode(),prefix/'cache/devstats/keep':b'cache',outside/'devstats':b'unrelated external data'}
for p,value in retained.items():p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes(value)
def unchanged():assert all(p.read_bytes()==value for p,value in retained.items())
dry=run(source/'uninstall-user.sh','--prefix',prefix,'--dry-run');assert 'Verified user installations to remove: 3' in dry.stdout;unchanged();checks.append('three marked versions discovered inside prefix')
# A forged recorded location must fail full preflight before any removal.
record=prefix/'opt/devstats-0.7.10/.devstats-user-paths';original=record.read_bytes();record.write_text('DevStats user paths v1\n'+str(prefix/'bin')+'\n'+str(outside)+'\n'+str(prefix/'config')+'\n')
run(source/'uninstall-user.sh','--prefix',prefix,ok=False);assert all((prefix/'opt'/('devstats-'+v)).exists() for v in ['0.7.9','0.7.10','0.7.11']);unchanged();record.write_bytes(original);checks.append('forged prefix-lookalike record rejected without any deletion')
# Symlink paths and dot traversal cannot redirect the prefix or a recorded root.
alias=root/'alias';alias.symlink_to(prefix,target_is_directory=True)
for script in [packages[-1]/'install-user.sh',source/'uninstall-user.sh']:
 for bad in ['', '/',str(alias),str(prefix)+'/../prefix 用户-other',str(prefix)+'//bin']:
  run(script,'--prefix',bad,ok=False);unchanged()
checks.append('empty/root/symlink/traversal/double-slash prefixes rejected')
run(source/'uninstall-user.sh','--prefix',prefix);assert not any((prefix/'opt'/('devstats-'+v)).exists() for v in ['0.7.9','0.7.10','0.7.11']);unchanged();checks.append('three versions removed while settings/history/cache and external same-name file preserved')
run(source/'uninstall-user.sh','--prefix',prefix);unchanged();checks.append('idempotent prefix uninstall')
print(json.dumps(dict(result='PASS',ordinaryUid=os.geteuid(),HOME_inherited=True,root=str(root),checks=checks,retainedFiles=len(retained))))
