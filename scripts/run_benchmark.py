#!/usr/bin/env python3
"""Matched loopback TCP experiment; raw client timing samples, no latency target."""
from __future__ import annotations
import argparse
import csv
import hashlib
import json
import math
from pathlib import Path
import platform
import re
import subprocess
import time
from run_demo import DemoCluster, values


def percentile(xs, q):
    return sorted(xs)[max(0, math.ceil(len(xs)*q)-1)]


def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--build-dir',type=Path,default=Path('build-release'))
    p.add_argument('--output',type=Path)
    p.add_argument('--samples',type=int,default=200)
    p.add_argument('--rounds',type=int,default=2)
    p.add_argument('--resource-bytes',type=int,default=64*1024*1024)
    a=p.parse_args()
    if not 100<=a.samples<=1000 or not 2<=a.rounds<=4: p.error('samples 100..1000, rounds 2..4')
    output=(a.output or a.build_dir/'benchmark').resolve();output.mkdir(parents=True,exist_ok=True)
    rows=[]; workloads=[]
    def sample(lab,client,command,workload,mode,run):
        r=client.command_result(command)
        rows.append(dict(run=run,mode=mode,workload=workload,ns=r['ns'],version=r.get('v0','')))
        return r
    for round_number in range(a.rounds):
        # Alternate AB and BA, keeping all build/transport/business paths equal.
        for mode in (('omit','checked') if round_number%2==0 else ('checked','omit')):
            run=f'round{round_number}-{mode}'
            with DemoCluster(a.build_dir,output/run,a.resource_bytes,0,'capstan_bench_host',['--validation',mode]) as lab:
                lab.a.command_result('shared');lab.a.command_result('fetch');lab.b.command_result('fetch')
                for _ in range(20):lab.a.command_result('version')
                before=values(lab.a.command_result('table_stats'))
                wire_before=values(lab.a.command_result('wire'))
                start=time.perf_counter_ns()
                for _ in range(a.samples):sample(lab,lab.a,'version','warm_version',mode,run)
                elapsed=time.perf_counter_ns()-start
                workloads.append(dict(run=run,workload='warm_version',count=a.samples,wall_ns=elapsed))
                for _ in range(10):sample(lab,lab.a,'summary','resource_summary',mode,run)
                summary_wire=values(lab.a.command_result('wire'))
                for _ in range(30):
                    am=lab.a.send('version');bm=lab.b.send('version')
                    for client,mark in ((lab.a,am),(lab.b,bm)):
                        r=client.wait_result('version',mark);assert r['status']=='ok'
                        rows.append(dict(run=run,mode=mode,workload='two_shared_readers',ns=r['ns'],version=r['v0']))
                after=values(lab.a.command_result('table_stats'));assert after==before
                workloads.append(dict(run=run,workload='resource_and_version_wire',resource_bytes=a.resource_bytes,
                                      bytes_delta=summary_wire[0]-wire_before[0],version_calls=a.samples,summary_calls=10,
                                      table_delta=[b-c for b,c in zip(after,before)]))
    # Authority handoff: separate workload, never folded into warm scalar cost.
    for interval in (1,10,100,1000):
        run=f'handoff-{interval}'
        with DemoCluster(a.build_dir,output/run,a.resource_bytes,0,'capstan_bench_host',['--validation','checked']) as lab:
            lab.a.command_result('exclusive 1');lab.a.command_result('fetch')
            holder=1; total=0;before=values(lab.a.command_result('stats'))
            start=time.perf_counter_ns()
            for turn in range(4 if interval<1000 else 2):
                client=lab.a if holder==1 else lab.b
                for _ in range(interval):
                    r=sample(lab,client,'add 0 1 1',f'write_every_{interval}','checked',run);total+=1
                    assert r['v0']==total
                holder=3-holder
                sample(lab,lab.a,f'exclusive {holder}',f'handoff_every_{interval}','checked',run)
                (lab.a if holder==1 else lab.b).command_result('fetch')
            after=values(lab.a.command_result('stats'))
            workloads.append(dict(run=run,workload='handoff',interval=interval,writes=total,
                                  wall_ns=time.perf_counter_ns()-start,control_rpc_delta=after[3]-before[3],
                                  drain_note='quiescent handoffs; nonzero drain separately evidenced in demo/fault logs'))
    # Actual overload sweep, retaining the same authenticated TCP/FF/read path.
    # Each connection has one pending slot; the second attempt is backpressure.
    overload=[]
    for concurrency in (1,8,32,80):
        run=f'overload-{concurrency}'
        with DemoCluster(a.build_dir,output/run,a.resource_bytes,5) as lab:
            lab.a.command_result('shared')
            hp=int(re.search(r'port=(\d+)',lab.host.lines[0]).group(1))
            tp=int(re.search(r'port=(\d+)',lab.table.lines[0]).group(1))
            load=lab.spawn('load','capstan_load','--credential',lab.role_paths[0],
                           '--host-port',hp,'--table-port',tp,'--concurrency',concurrency)
            peak_rss=0;peak_active=0;deadline=time.monotonic()+20
            while load.process.poll() is None:
                if time.monotonic()>deadline: raise TimeoutError("overload worker did not drain")
                try:
                    text=Path(f'/proc/{lab.host.process.pid}/status').read_text()
                    peak_rss=max(peak_rss,int(re.search(r'VmRSS:\s+(\d+)',text).group(1)))
                    for line in lab.host.lines:
                        match=re.search(r'active=(\d+)',line)
                        if match:peak_active=max(peak_active,int(match.group(1)))
                except (FileNotFoundError,AttributeError):pass
                time.sleep(.01)
            load.reader.join(timeout=2)
            assert load.process.returncode==0, load.lines[-10:]
            line=next(x for x in load.lines if x.startswith('LOAD '))
            record={k:int(v) for k,v in (item.split('=') for item in line.split()[1:])}
            record.update(host_peak_rss_kib=peak_rss,observed_peak_active=peak_active)
            assert peak_active<=64 and record['ok']<=min(concurrency,62)
            record['backpressure_note']='Second pending call per connection rejected locally; above the server session limit connections may be rejected before submit or finish Unknown. No timeout releases Host pins.'
            overload.append(record)
            for line in load.lines:
                if line.startswith('SAMPLE '):rows.append(dict(run=run,mode='checked',workload='overload_'+str(concurrency),ns=int(line.split('=')[1]),version=''))
    with (output/'raw.csv').open('w',newline='') as f:
        writer=csv.DictWriter(f,fieldnames=['run','mode','workload','ns','version']);writer.writeheader();writer.writerows(rows)
    groups={}
    for r in rows:groups.setdefault((r['workload'],r['mode']),[]).append(r['ns'])
    summary={f'{workload}/{mode}':dict(n=len(xs),p50_ns=percentile(xs,.5),p95_ns=percentile(xs,.95),p99_ns=percentile(xs,.99)) for (workload,mode),xs in groups.items()}
    baseline=summary['warm_version/omit'];checked=summary['warm_version/checked']
    increment={p:checked[p]-baseline[p] for p in ('p50_ns','p95_ns','p99_ns')}
    cache=(a.build_dir/'CMakeCache.txt').read_text()
    flags=[line for line in cache.splitlines() if line.startswith(('CMAKE_BUILD_TYPE:','CMAKE_CXX_FLAGS:','CMAKE_CXX_FLAGS_RELEASE:','CAPSTAN_ASAN:','CAPSTAN_TSAN:','CAPSTAN_NO_EXCEPTIONS:','CAPSTAN_NO_EXCEPTION_STRICT:','CAPSTAN_UV_WORKER:','CAPSTAN_FAST_RUNNER:','CAPSTAN_LTO:','CAPSTAN_NATIVE_ARCH:'))]
    artifacts={name:hashlib.sha256((a.build_dir/name).read_bytes()).hexdigest() for name in ('capstan_bench_host','capstan_table','capstan_client','capstan_host','capstan_load')}
    report=dict(passed=True,command_exit_code=0,platform=platform.platform(),machine=platform.machine(),
                compiler=subprocess.check_output(['c++','--version'],text=True).splitlines()[0],
                build_flags=flags,binary_sha256=artifacts,dependencies=json.loads(Path('deps.lock.json').read_text()),
                samples_per_round=a.samples,rounds=a.rounds,order='AB then BA, repeated if requested',summary=summary,
                grant_validation_increment_ns=increment,workloads=workloads,overload=overload,
                scope='Same executable, protocol, GIO TCP, Dynabridge typed object/codec, FF path, Lease, serial lane, authentication envelope and business operation; omit skips only Host grant/effect admission validation.',
                limitations=['Localhost on one machine; no cross-machine result.',
                             'Wall throughput includes Python driver/stdin pacing; client ns excludes driver round trip.',
                             'Host/client stdout logging enabled identically; scheduler and logging may dominate the small validation increment.',
                             'No significance claim for small or negative quantile differences; p99 has few tail samples.',
                             'Summary has only 10 samples per round; its p99 is descriptive, not a reliable tail estimate.',
                             'Shared readers execute serially; no parallel-read claim.',
                             'Overload RSS is a 10ms observed high-water sample, not a proof of total memory bound; queue/session limits are separately tested.',
                             'Overload percentiles combine successes and local rejection attempts; they are not service-only latency.',
                             'Cancellation capability is identical in both modes; neither retries writes.'])
    for w in report['workloads']:
        if 'count'in w:w['driver_throughput_per_s']=w['count']*1e9/w['wall_ns']
        if 'writes'in w:w['driver_throughput_per_s']=w['writes']*1e9/w['wall_ns']
    (output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(dict(passed=True,output=str(output),warm_baseline=baseline,warm_checked=checked,increment_ns=increment),indent=2))
    return 0
if __name__=='__main__':raise SystemExit(main())
