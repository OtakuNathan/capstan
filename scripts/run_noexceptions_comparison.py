#!/usr/bin/env python3
"""Matched EH / strict / relaxed no-exception comparison, including in-flight handoff."""
import argparse,csv,hashlib,json
from pathlib import Path
from run_demo import DemoCluster,values
from run_benchmark import percentile

parser=argparse.ArgumentParser(description=__doc__)
for name in ['eh','strict','relaxed']:
    parser.add_argument('--'+name,type=Path,required=True)
parser.add_argument('--output',type=Path,required=True)
a=parser.parse_args();a.output.mkdir(parents=True,exist_ok=True)
modes=['eh','strict','relaxed'];rows=[];runs=[]
expected_summary=None
for iteration in range(len(modes)):
    for mode in modes[iteration:]+modes[:iteration]:
        build=getattr(a,mode);name=f'{iteration}-{mode}'
        with DemoCluster(build,a.output/name,64*1024*1024,0) as lab:
            lab.a.command_result('shared');lab.a.command_result('fetch')
            for _ in range(30):lab.a.command_result('version')
            for _ in range(5):
                result=values(lab.a.command_result('summary'))
                if expected_summary is None:expected_summary=result
                assert result==expected_summary
            table_before=values(lab.a.command_result('table_stats'))
            for workload,count in [('version',2000),('summary',100)]:
                xs=[]
                for index in range(count):
                    result=lab.a.command_result(workload);xs.append(result['ns'])
                    if workload=='summary':assert values(result)==expected_summary
                    else:assert result['v0']==0
                    rows.append(dict(run=name,mode=mode,workload=workload,sample=index,ns=result['ns'],pending=False))
                runs.append(dict(run=name,mode=mode,workload=workload,n=count,**{f'p{int(q*100)}_ns':percentile(xs,q) for q in [.5,.95,.99]}))
            assert values(lab.a.command_result('table_stats'))==table_before
            lab.a.command_result('exclusive 2');lab.b.command_result('fetch')
            controls=lab.a.command_result('table_stats')['v0']
            xs=[]
            for index in range(100):
                mark=lab.host.mark();read=lab.b.send('summary')
                admitted,_=lab.host.wait_line(lambda s:s.startswith('ADMIT '),mark)
                transfer=lab.a.send('exclusive 1')
                _,line=lab.host.wait_line(lambda s:s.startswith('TRANSITION '),admitted)
                handoff=lab.a.wait_result('exclusive',transfer);result=lab.b.wait_result('summary',read)
                assert handoff['status']==result['status']=='ok' and values(result)==expected_summary
                rows.append(dict(run=name,mode=mode,workload='handoff',sample=index,ns=handoff['ns'],pending='status=pending' in line))
                xs.append(handoff['ns'])
                lab.a.command_result('exclusive 2');lab.b.command_result('fetch')
            assert lab.a.command_result('table_stats')['v0']-controls==200
            runs.append(dict(run=name,mode=mode,workload='handoff',n=100,**{f'p{int(q*100)}_ns':percentile(xs,q) for q in [.5,.95,.99]}))
            stats=values(lab.a.command_result('stats'));assert stats[0]==stats[1]==2235 and stats[4]==0
        print(name,'version',runs[-3]['p50_ns'],runs[-3]['p99_ns'],'summary',runs[-2]['p50_ns'],'handoff',runs[-1]['p50_ns'],flush=True)
with (a.output/'raw.csv').open('w') as f:
    writer=csv.DictWriter(f,fieldnames=list(rows[0]));writer.writeheader();writer.writerows(rows)
summary={}
for mode in modes:
    for workload in ['version','summary','handoff']:
        xs=[r['ns'] for r in rows if r['mode']==mode and r['workload']==workload]
        summary[f'{mode}/{workload}']=dict(n=len(xs),pending=sum(r['pending'] for r in rows if r['mode']==mode and r['workload']==workload),**{f'p{int(q*100)}_ns':percentile(xs,q) for q in [.5,.95,.99]})
report=dict(passed=True,summary=summary,runs=runs,expected_summary=expected_summary,
    binary_sha256={m:{n:hashlib.sha256((getattr(a,m)/n).read_bytes()).hexdigest() for n in ['capstan_host','capstan_client','capstan_table']} for m in modes},
    build_flags={m:[line for line in (getattr(a,m)/'CMakeCache.txt').read_text().splitlines() if line.startswith(('CMAKE_BUILD_TYPE:','CMAKE_CXX_FLAGS_RELEASE:','CAPSTAN_LTO:','CAPSTAN_NATIVE_ARCH:','CAPSTAN_FAST_RUNNER:','CAPSTAN_UV_WORKER:','CAPSTAN_VECTORIZATION_REPORT:','CAPSTAN_NO_EXCEPTIONS:','CAPSTAN_NO_EXCEPTION_STRICT:'))] for m in modes},
    dependencies=json.loads(Path('deps.lock.json').read_text()),
    scope='Three rotated orders, same source/compiler/O3/LTO/native/debug symbols, libuv worker, GIO TCP, full grants, protocol/codec/Lease/serial lane, 64MiB data, normal FF runner, no requested cancellation. Only exception compilation and static policy differ. All results checked equal; one ACK RPC per transition.',
    limitations=['One machine, Python paced, logging enabled, no saturation throughput claim.',
        'No-exception modes have fail-stop general allocator exhaustion; bounded rejection, protocol errors and FF nothrow submission failures remain explicit results.',
        'Strict/relaxed relax selected static constraints; mandatory Lease topology and FF transform/awaitable contracts remain enforced.',
        'Power policy unchanged; descriptive three-round percentiles, not a significance claim.'])
(a.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(json.dumps(summary,indent=2))
