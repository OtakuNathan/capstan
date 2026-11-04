#!/usr/bin/env python3
import argparse
from pathlib import Path
import subprocess
from compile_policy import flags
p=argparse.ArgumentParser()
p.add_argument('--compiler',required=True)
p.add_argument('--root',type=Path,required=True)
p.add_argument('--build',type=Path,required=True)
a=p.parse_args()
for name,expected in [('exclusive_copy','deleted'),('readonly_write','add_range'),('wrong_receiver','conver')]:
    source=a.root/'tests/compile'/('p2_'+name+'.cpp')
    includes=[a.root,a.root/'include',a.build/'_deps/dynabridge',a.build/'_deps/flux_foundry',a.build/'_deps/lease/lease']
    command=[a.compiler,'-std=c++14','-fsyntax-only']+flags(a.build)+['-I'+str(i) for i in includes]+[str(source)]
    result=subprocess.run(command,text=True,capture_output=True)
    folder=a.build/'compile-negative';folder.mkdir(exist_ok=True)
    (folder/('p2_'+name+'.log')).write_text(repr(command)+'\nexit='+str(result.returncode)+'\n'+result.stdout+result.stderr)
    if result.returncode==0 or expected not in result.stderr.lower():
        raise SystemExit('unexpected negative compilation: '+name+'\n'+result.stderr)
    print('PASS p2_'+name+' exit='+str(result.returncode))
