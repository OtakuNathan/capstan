#!/usr/bin/env python3
"""ABBA Table administration vs conditional holder transfer, including recipient readiness."""
import argparse
import json
import math
import platform
import time
from pathlib import Path
from run_demo import DemoCluster,values
from run_direct_transfer import reference


def stats(samples):
    samples=sorted(samples)
    def percentile(q):return samples[max(0,math.ceil(len(samples)*q)-1)]/1000
    return dict(n=len(samples),unit='us',p50=percentile(.5),p95=percentile(.95),p99=percentile(.99),max=samples[-1]/1000)


def run(build,output,samples):
    rows=[];blocks=[];inflight=[];warm=[]
    for index,kind in enumerate(['table','direct','direct','table']):
        with DemoCluster(build,output/f'{index}-{kind}',work_delay_ms=0) as lab:
            a,b=lab.a,lab.b;a.command_result('exclusive 1');a.command_result('fetch')
            original=a.command_result('summary');holder=1
            before=None
            for n in range(samples+8):
                source,target=(a,b) if holder==1 else (b,a)
                recipient=3-holder
                if n==8:before=a.command_result('table_stats')
                begin=time.monotonic_ns()
                if kind=='direct':
                    installed=source.command_result(f'transfer {recipient} {n+1}')
                    ready=target.command_result('claim '+reference(installed))
                    epoch=installed['v7']
                else:
                    installed=a.command_result(f'exclusive {recipient}')
                    ready=target.command_result('fetch');epoch=installed['v0']
                elapsed=time.monotonic_ns()-begin
                first=target.command_result('version')
                assert ready['v0']==epoch
                if n>=8:rows.append(dict(block=index,kind=kind,direction=f'{holder}->{recipient}',iteration=n-8,
                    installed_ns=installed['ns'],claim_or_fetch_ns=ready['ns'],recipient_ready_components_ns=installed['ns']+ready['ns'],
                    recipient_ready_wall_ns=elapsed,first_call_ns=first['ns'],epoch=epoch))
                holder=recipient
            after=a.command_result('table_stats')
            delta=[x-y for x,y in zip(values(after),values(before))]
            assert delta[:3]==([4*samples,samples,0] if kind=='table' else [0,0,0])
            current=a if holder==1 else b
            assert values(current.command_result('summary'))==values(original)
            warm_before=a.command_result('table_stats')
            for i in range(100):warm.append(dict(block=index,kind=kind,ns=current.command_result('version')['ns']))
            assert values(a.command_result('table_stats'))==values(warm_before)
            blocks.append(dict(index=index,kind=kind,table_delta=delta,resource_unchanged=True))
    # No injected work delay: verify the summary is actually admitted and the
    # transfer actually Pending. All segment timestamps come from this Host.
    with DemoCluster(build,output/'inflight',work_delay_ms=0) as lab:
        a,b=lab.a,lab.b;a.command_result('exclusive 1');a.command_result('fetch');holder=1
        for n in range(20):
            source,target=(a,b) if holder==1 else (b,a)
            mark=lab.host.mark();business=source.send('summary')
            lab.host.wait_line(lambda s:s.startswith('ADMIT '),mark)
            transfer=source.send(f'transfer {3-holder} {1000+n}')
            _,line=lab.host.wait_line(lambda s:s.startswith('TRANSFER ') and 'status=pending' in s,mark)
            installed=source.wait_result('transfer',transfer);assert installed['status']=='ok'
            completed=source.wait_result('summary',business);assert completed['status']=='ok'
            ready=target.command_result('claim '+reference(installed));first=target.command_result('version')
            _,line=lab.host.wait_line(lambda s:s.startswith('TRANSFER_TIMING ') and f'tx={1000+n} ' in s,mark)
            t={k:int(v) for k,v in (field.split('=') for field in line.split()[1:])}
            assert t['close_ns']<=t['drain_ns']<=t['install_ns']<=t['reply_ns']
            inflight.append(dict(iteration=n,installed_ns=installed['ns'],claim_ns=ready['ns'],first_call_ns=first['ns'],
                close_to_drain_ns=t['drain_ns']-t['close_ns'],drain_to_install_ns=t['install_ns']-t['drain_ns'],
                install_to_reply_ns=t['reply_ns']-t['install_ns'],host_timestamps=t))
            holder=3-holder
    summary={kind:{field:stats([r[field] for r in rows if r['kind']==kind]) for field in
             ['installed_ns','claim_or_fetch_ns','recipient_ready_components_ns','recipient_ready_wall_ns','first_call_ns']} for kind in ['table','direct']}
    summary['warm_version']={kind:stats([r['ns'] for r in warm if r['kind']==kind]) for kind in ['table','direct']}
    summary['inflight']={field:stats([r[field] for r in inflight]) for field in ['installed_ns','claim_ns','first_call_ns','close_to_drain_ns','drain_to_install_ns','install_to_reply_ns']}
    return dict(passed=True,order=['table','direct','direct','table'],build=str(build.resolve()),platform=platform.platform(),
        resource_bytes=64*1024*1024,work_delay_ms=0,samples_per_block=samples,warmup_transfers_per_block=8,
        rows=rows,warm=warm,inflight=inflight,blocks=blocks,summary=summary,
        semantics={'table':'administrative reassignment; Host snapshot + expected epoch; post-ACK observation; recipient fetch queries Host',
                   'direct':'conditional transfer by current exclusive holder; Installed receipt then authenticated recipient claim',
                   'recipient_ready_components':'sum of two client-local command durations, includes encoding, FF, RPC and materialization; excludes Python gap',
                   'recipient_ready_wall':'driver end-to-end elapsed including Python dispatch and scheduling between installed and recipient-ready',
                   'host_segments':'single Host steady clock; drain timestamp is observation of last pin completion in decision domain',
                   'p99':'descriptive sample quantile, no stable tail-latency or cross-machine claim'})


def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--build-dir',type=Path,required=True);p.add_argument('--output',type=Path,required=True);p.add_argument('--samples',type=int,default=100);args=p.parse_args()
    args.output.mkdir(parents=True,exist_ok=True)
    try:report=run(args.build_dir,args.output,args.samples);code=0
    except Exception as e:report=dict(passed=False,error=repr(e));code=1
    report['command_exit_code']=code;(args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:v for k,v in report.items() if k not in ('rows','warm','inflight')},indent=2));return code
if __name__=='__main__':raise SystemExit(main())
