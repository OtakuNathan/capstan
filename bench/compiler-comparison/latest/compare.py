#!/usr/bin/env python3
"""Matched compiler/toolchain comparison, three ABBA cycles, loopback TCP."""
import csv,hashlib,json,math,os,platform,statistics,subprocess,sys,time
from pathlib import Path
ROOT=Path(__file__).resolve().parent
sys.path.insert(0,str(ROOT/'scripts'))
from run_demo import DemoCluster,values
from run_direct_transfer import reference
OUT=ROOT/'results/comparison';OUT.mkdir(parents=True,exist_ok=True)
SHA='bf9c80e60d616b0a15717f68e7a72850dbdc02ae'

def require(ok,message):
    if not ok:raise AssertionError(message)

def describe(samples):
    ordered=sorted(samples)
    return dict(n=len(samples),unit='us',p50=ordered[math.ceil(len(samples)*.5)-1]/1000,
                p95=ordered[math.ceil(len(samples)*.95)-1]/1000,p99=ordered[math.ceil(len(samples)*.99)-1]/1000)

def machine():
    return dict(loadavg=Path('/proc/loadavg').read_text().strip(),
                frequency_khz={p.parent.parent.name:p.read_text().strip() for p in Path('/sys/devices/system/cpu').glob('cpu[0-9]*/cpufreq/scaling_cur_freq')})

def run():
    all_rows=[];blocks=[];expected=None
    with (OUT/'raw.csv').open('w') as stream:
        writer=csv.DictWriter(stream,fieldnames=['block','compiler','path','metric','sample','ns']);writer.writeheader()
        for index,label in enumerate(['gcc','clang','clang','gcc']*3):
            name=f'{index:02d}-{label}';rows=[];environment=machine()
            def record(path,metric,n,ns):
                row=dict(block=index,compiler=label,path=path,metric=metric,sample=n,ns=ns)
                rows.append(row);all_rows.append(row);writer.writerow(row)
            with DemoCluster(ROOT/('build-'+label),OUT/name,64*1024*1024,0) as lab:
                a,b=lab.a,lab.b;a.command_result('exclusive 1');a.command_result('fetch')
                result=values(a.command_result('summary'))
                if expected is None:expected=result
                require(result==expected,'summary differs between compilers or blocks')
                before=values(a.command_result('table_stats'))
                workloads=[('version',1000,50),('summary',100,10)]
                if index%2:workloads.reverse()
                for command,count,warmup in workloads:
                    for _ in range(warmup):a.command_result(command)
                    for n in range(count):
                        r=a.command_result(command)
                        require(values(r)==(expected if command=='summary' else [0]),'business result mismatch')
                        record('business',command,n,r['ns'])
                require(values(a.command_result('table_stats'))==before,'warm calls touched Table')
                holder=1
                paths=['table','direct'] if index%2==0 else ['direct','table']
                for path in paths:
                    before=None
                    for n in range(68):
                        source,target=(a,b) if holder==1 else (b,a)
                        if n==8:before=values(a.command_result('table_stats'))
                        start=time.monotonic_ns()
                        if path=='direct':
                            installed=source.command_result(f'transfer {3-holder} {n+1}')
                            recipient=target.command_result('claim '+reference(installed))
                            epoch=installed['v7']
                        else:
                            installed=a.command_result(f'exclusive {3-holder}')
                            recipient=target.command_result('fetch');epoch=installed['v0']
                        elapsed=time.monotonic_ns()-start
                        require(recipient['v0']==epoch,'recipient epoch mismatch')
                        first=target.command_result('version');require(first['v0']==0,'handoff changed resource version')
                        if n>=8:
                            for metric,ns in [('installed',installed['ns']),('claim_or_fetch',recipient['ns']),
                                              ('ready_components',installed['ns']+recipient['ns']),('ready_wall',elapsed),('first_call',first['ns'])]:
                                record(path,metric,n-8,ns)
                        holder=3-holder
                    after=values(a.command_result('table_stats'))
                    require([x-y for x,y in zip(after,before)][:3]==([240,60,0] if path=='table' else [0,0,0]),'unexpected Table traffic')
                require(values(a.command_result('summary'))==expected,'data changed across transfers')
                host=a.command_result('stats');require(host['v0']==host['v1'] and host['v4']==0,'Host work did not drain')
            metrics={}
            for path,metric in sorted({(r['path'],r['metric']) for r in rows}):
                metrics[path+'/'+metric]=describe([r['ns'] for r in rows if (r['path'],r['metric'])==(path,metric)])
            blocks.append(dict(block=index,compiler=label,metrics=metrics,environment_before=environment,environment_after=machine()))
            stream.flush()
            print(name,'version',metrics['business/version']['p50'],'summary',metrics['business/summary']['p50'],
                  'direct ready',metrics['direct/ready_components']['p50'],flush=True)
            (OUT/'blocks.json').write_text(json.dumps(blocks,indent=2)+'\n')
    summary={label:{key:describe([r['ns'] for r in all_rows if r['compiler']==label and r['path']+'/'+r['metric']==key])
                    for key in blocks[0]['metrics']} for label in ['gcc','clang']}
    # Pair the two compiler medians within each ABBA cycle instead of treating
    # individual requests as independent experimental replications.
    ratios={key:[] for key in blocks[0]['metrics']}
    for cycle in range(3):
        group=blocks[cycle*4:cycle*4+4]
        for key in ratios:
            g=statistics.median(b['metrics'][key]['p50'] for b in group if b['compiler']=='gcc')
            c=statistics.median(b['metrics'][key]['p50'] for b in group if b['compiler']=='clang')
            ratios[key].append(c/g)
    binaries={};flags={};sizes={}
    for label in ['gcc','clang']:
        build=ROOT/('build-'+label)
        binaries[label]={role:hashlib.sha256((build/('capstan_'+role)).read_bytes()).hexdigest() for role in ['host','table','client']}
        flags[label]=[line for line in (build/'CMakeCache.txt').read_text().splitlines() if line.startswith(('CMAKE_BUILD_TYPE:','CMAKE_CXX_COMPILER:','CMAKE_CXX_FLAGS_RELEASE:','CMAKE_EXE_LINKER_FLAGS:','CAPSTAN_FAST_RUNNER:','CAPSTAN_NO_EXCEPTIONS:','CAPSTAN_NO_EXCEPTION_STRICT:','CAPSTAN_UV_WORKER:','CAPSTAN_LTO:','CAPSTAN_NATIVE_ARCH:'))]
        sizes[label]=subprocess.check_output(['size',str(build/'capstan_host'),str(build/'capstan_client'),str(build/'capstan_table')],text=True)
    return dict(passed=True,source_commit=SHA,source_archive_sha256=hashlib.sha256((ROOT/'source.tar.gz').read_bytes()).hexdigest(),
                script_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),order=['gcc','clang','clang','gcc']*3,
                resource_bytes=64*1024*1024,work_delay_ms=0,platform=platform.platform(),
                cpu=next(line.split(':',1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines() if line.startswith('model name')),
                versions={label:subprocess.check_output([compiler,'--version'],text=True).splitlines()[0] for label,compiler in [('gcc','g++'),('clang','clang++')]},
                build_flags=flags,binary_sha256=binaries,binary_sections=sizes,expected_summary=expected,summary=summary,
                clang_over_gcc_block_median_ratios=ratios,blocks=blocks,
                semantics={'installed':'Source command duration; Table administration includes post-ACK Host observation.',
                           'ready_components':'Per-transfer sum of Installed and claim/fetch command durations, including recipient materialization; excludes Python gap.',
                           'ready_wall':'Driver wall time including Python dispatch gap and scheduling.',
                           'summary':'End-to-end RPC scanning the same seeded 64 MiB Dataset and checking count/sum/checksum/version.',
                           'toolchains':'GCC with GNU linker vs Clang with lld; both use LTO. The linker is part of this comparison.'},
                limitations=['Single 3700X, loopback TCP, Python paced, logging enabled; no saturation or cross-machine claim.',
                             'No CPU affinity or power policy changes. ABBA controls ordering but does not eliminate scheduling/frequency variance.',
                             'All requests complete before the next authority handoff; transfer comparison is quiescent, not in-flight Drain.',
                             'Three ABBA cycles are descriptive replication, not a general compiler ranking or a significance claim.'])

try:
    report=run();code=0
except Exception as error:
    report=dict(passed=False,error=repr(error));code=1
report['command_exit_code']=code
(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps({k:v for k,v in report.items() if k in ('passed','error','summary','clang_over_gcc_block_median_ratios')},indent=2),flush=True)
raise SystemExit(code)
