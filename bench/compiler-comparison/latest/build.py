import json,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parent;out=root/'results';out.mkdir(exist_ok=True)
common=['-DCMAKE_BUILD_TYPE=Release','-DCAPSTAN_NO_EXCEPTIONS=ON','-DCAPSTAN_NO_EXCEPTION_STRICT=ON',
        '-DCAPSTAN_FAST_RUNNER=ON','-DCAPSTAN_UV_WORKER=ON','-DCAPSTAN_LTO=ON','-DCAPSTAN_NATIVE_ARCH=ON',
        f'-DCAPSTAN_LEASE_SOURCE={root}/build-deps/lease',
        f'-DCAPSTAN_DYNABRIDGE_SOURCE={root}/build-deps/dynabridge',
        f'-DCAPSTAN_FF_SOURCE={root}/build-deps/flux_foundry']
records=[]
for label,compiler,extra in [('gcc','g++',[]),('clang','clang++',['-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld'])]:
    commands=[('configure',['cmake','-S','.', '-B','build-'+label,*common,'-DCMAKE_CXX_COMPILER='+compiler,*extra]),
              ('build',['cmake','--build','build-'+label,'-j','8','--target','capstan_host','capstan_table','capstan_client']),
              ('test',['ctest','--test-dir','build-'+label,'-R','^(four_process_demo|direct_transfer_demo|tcp_direct_transfer)$','--output-on-failure'])]
    for stage,command in commands:
        start=time.monotonic()
        with (out/f'{label}-{stage}.log').open('w') as log:
            code=subprocess.run(command,cwd=root,stdout=log,stderr=subprocess.STDOUT).returncode
        records.append(dict(mode=label,stage=stage,command=command,exit_code=code,seconds=time.monotonic()-start))
        (out/'builds.json').write_text(json.dumps(records,indent=2)+'\n')
        print(label,stage,code,flush=True)
        if code:raise SystemExit(code)
