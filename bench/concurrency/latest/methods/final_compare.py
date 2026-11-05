import collections,csv,hashlib,json,math,random,statistics,sys
from pathlib import Path
ROOT=Path.cwd()
sys.path.insert(0,str(ROOT/'scripts'))
from run_demo import DemoCluster,values
OUT=ROOT/'final-comparison';OUT.mkdir(exist_ok=True)
configs={
 'gsocket-inherited':('capstan_host_inherited','capstan_throughput_inherited',0),
 'gsocket-worker':('capstan_host_reference','capstan_throughput_reference',0),
 'fd-worker':('capstan_host_unguarded','capstan_throughput',0),
 'fd-inline-unguarded':('capstan_host_unguarded','capstan_throughput',1),
 'fd-inline':('capstan_host','capstan_throughput',1),
}
runs=[]
report=dict(passed=False,source_sha256={name:hashlib.sha256((ROOT/name).read_bytes()).hexdigest() for name in ['src/apps/host.cpp','src/apps/throughput.cpp','src/apps/common.hpp','src/apps/authority_replies.hpp','src/transport/gio.cpp','include/capstan/transport/context_dispatch.hpp','deps.lock.json','build_reference.py','final_compare.py']},configurations={k:dict(host=v[0],load=v[1],inline_version=v[2],trace_requests=0,history_limit=65536,
            host_sha256=hashlib.sha256((ROOT/'build'/v[0]).read_bytes()).hexdigest(),
            load_sha256=hashlib.sha256((ROOT/'build'/v[1]).read_bytes()).hexdigest()) for k,v in configs.items()},runs=runs,
            reference='Current Host/driver with identical callback publication. gsocket-inherited retains the old five-second data socket timeout; gsocket-worker clears it like the candidate. Both use worker dispatch. All controls retain unconditional construction of the ACK reply array; only fd-inline includes the empty-waiter fast return. No grant, pin, dedup or Drain checks omitted.',
            scope='One shared 64 MiB Dataset, native closed-loop finite batches. History limit 65536 in all configurations; no retirement or indefinite-capacity claim.')
def run(label,n,repeat,workload='version'):
    host,driver,inline=configs[label];name=f'{workload}-r{repeat}-c{n}-{label}';folder=OUT/name
    count,warmup=(50000,1000) if workload=='version' else (512,8)
    with DemoCluster(ROOT/'build',folder,64*1024*1024,0,host,['--trace-requests',0,'--inline-version',inline,'--history-limit',65536]) as lab:
        lab.a.command_result('shared');lab.a.command_result('fetch');expected=values(lab.a.command_result('summary'))
        load=lab.spawn('load',driver,'--credential',lab.role_paths[1],
                       '--host-port',lab.host_port,'--table-port',lab.table_port,
                       '--workload',workload,'--concurrency',n,'--count',count,'--warmup',warmup,
                       '--expected-count',expected[0],'--expected-sum',expected[1],'--expected-checksum',expected[2],
                       '--output',folder/'samples.csv')
        load.process.wait(timeout=60);load.reader.join(timeout=2);assert load.process.returncode==0,load.lines
        fields=dict(part.split('=') for part in next(s for s in load.lines if s.startswith('LOAD ')).split()[1:])
        stats={k:int(v) for k,v in fields.items()};assert stats['ok']==count and stats['warm_errors']==0 and stats['errors']==0,stats
        assert values(lab.a.command_result('summary'))==expected
        host_stats=values(lab.a.command_result('stats'));assert host_stats[0]==host_stats[1]==count+warmup+2 and host_stats[4]==0
        samples=sorted(int(x['ns']) for x in csv.DictReader((folder/'samples.csv').open()) if x['valid']=='1');assert len(samples)==count
        result=dict(label=label,workload=workload,concurrency=n,repeat=repeat,name=name,stats=stats,
                    rps=count*1e9/stats['wall_ns'],latency={f'p{q}_us':samples[math.ceil(count*q/100)-1]/1000 for q in [50,95,99]},
                    host_stats=host_stats,loadavg=Path('/proc/loadavg').read_text().strip())
    print(name,round(result['rps']),result['latency'],flush=True)
    return result
try:
    for workload,levels in [('version',[1,2,4,8,16,32,48]),('summary',[1,8,32])]:
        for repeat in range(3):
            order=list(levels)
            if repeat%2:order.reverse()
            elif repeat:random.Random(3100+repeat).shuffle(order)
            for n in order:
                labels=list(configs);random.Random(3100+repeat*100+n).shuffle(labels)
                for label in labels:
                    runs.append(run(label,n,repeat,workload));(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    summary={}
    for workload in ['version','summary']:
        for label in configs:
            for n in sorted({x['concurrency'] for x in runs if x['workload']==workload}):
                rows=[x for x in runs if x['label']==label and x['concurrency']==n and x['workload']==workload]
                xs=[]
                for row in rows:xs.extend(int(x['ns']) for x in csv.DictReader((OUT/row['name']/'samples.csv').open()))
                xs.sort();rates=[x['rps'] for x in rows]
                summary[f'{workload}/{label}/{n}']=dict(n=len(xs),rps=statistics.median(rates),rps_min=min(rates),rps_max=max(rates),
                            **{f'p{q}_us':xs[math.ceil(len(xs)*q/100)-1]/1000 for q in [50,95,99]})
    report.update(passed=True,summary=summary)
except BaseException as error:
    report.update(passed=False,error=repr(error));raise
finally:(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
for key,row in report['summary'].items():print('FINAL',key,round(row['rps']),round(row['p99_us'],2),flush=True)
