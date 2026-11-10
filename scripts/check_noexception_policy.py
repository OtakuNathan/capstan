#!/usr/bin/env python3
import argparse,json,subprocess
from pathlib import Path
p=argparse.ArgumentParser()
for name in ['compiler','root','build']:p.add_argument('--'+name,required=True)
a=p.parse_args();root=Path(a.root);build=Path(a.build)
out=build/'noexception-policy';out.mkdir(exist_ok=True)
rows=[]
for library in ['ff','bridge','migration','owner']:
    for strict in [False,True]:
        for annotated in [False,True]:
            name=f'{library}-strict{int(strict)}-noexcept{int(annotated)}'
            command=[a.compiler,'-std=c++14','-fno-exceptions','-fsyntax-only','-ftemplate-backtrace-limit=1',
                '-DFLUX_FOUNDRY_NO_EXCEPTION_STRICT='+str(int(strict)),
                '-DCAPSTAN_NO_EXCEPTION_STRICT='+str(int(strict)),
                '-DDYNABRIDGE_NO_EXCEPTION_STRICT='+str(int(strict)),
                '-I'+str(root/'include'),'-I'+str(build/'_deps/flux_foundry'),'-I'+str(build/'_deps/dynabridge')]
            if library=='bridge':command+=['-DPROBE_BRIDGE=1']
            if library=='migration':command+=['-DPROBE_MIGRATION=1']
            if library=='owner':command+=['-DPROBE_OWNER=1']
            if annotated:command+=['-DPROBE_NOEXCEPT=1']
            command+=[str(root/'tests/compile/noexception_policy.cpp')]
            result=subprocess.run(command,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
            expected_failure=strict and not annotated
            passed=(result.returncode!=0 and 'static assertion failed' in result.stdout and ('noexcept' in result.stdout or 'nothrow' in result.stdout)) if expected_failure else result.returncode==0
            rows.append(dict(name=name,command=command,exit_code=result.returncode,expected_failure=expected_failure,passed=passed))
            (out/(name+'.log')).write_text(result.stdout)
            if not passed:print(result.stdout)
(out/'report.json').write_text(json.dumps(rows,indent=2)+'\n')
assert all(r['passed'] for r in rows),rows
print('PASS: strict rejects unannotated extensions; relaxed accepts; annotated accepted in both, FF, Dynabridge, migration validation and owner backends')
