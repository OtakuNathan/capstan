import json
from pathlib import Path
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
ROOT=Path('bench/concurrency/latest');report=json.loads((ROOT/'report.json').read_text())
labels={'gsocket-inherited':'GSocket + inherited timeout + worker','gsocket-worker':'GSocket + cleared timeout + worker','fd-worker':'FD source + worker','fd-inline-unguarded':'FD source + inline (unconditional ACK array)','fd-inline':'FD source + inline + empty ACK guard'}
colors=['#8b949e','#b27c36','#4579c5','#9366ab','#138b72']
fig,axes=plt.subplots(1,2,figsize=(12,4.8),layout='constrained')
for (label,title),color in zip(labels.items(),colors):
    rows=[(int(k.split('/')[-1]),v) for k,v in report['summary'].items() if k.startswith('version/'+label+'/')];rows.sort()
    x=[r[0] for r in rows];y=[r[1]['rps']/1000 for r in rows]
    axes[0].plot(x,y,'o-',label=title,color=color,linewidth=2,markersize=4)
    axes[0].fill_between(x,[r[1]['rps_min']/1000 for r in rows],[r[1]['rps_max']/1000 for r in rows],color=color,alpha=.1)
    axes[1].plot(x,[r[1]['p99_us']/1000 for r in rows],'o-',color=color,linewidth=2,markersize=4)
for axis in axes:
    axis.set_xlabel('Concurrent connections (one outstanding RPC each)')
    axis.set_xticks([1,4,8,16,32,48]);axis.grid(alpha=.2)
    axis.spines[['top','right']].set_visible(False)
axes[0].set_ylabel('Successful version RPCs / second (thousands)');axes[0].set_ylim(bottom=0)
axes[1].set_ylabel('Pooled p99 latency (ms)');axes[1].set_ylim(bottom=0)
fig.suptitle('Capstan on Ryzen 7 3700X: native loopback version RPC',fontsize=14)
axes[0].legend(fontsize=8,loc='lower left')
fig.supxlabel('3 rounds × 50,000 measured calls per point; all traces off; finite history 65,536.\nRate: median (band: min–max). Latency: pooled samples. Full admission, dedup and Drain retained.',fontsize=9)
fig.savefig(ROOT/'version-concurrency.png',dpi=180)
fig.savefig(ROOT/'version-concurrency.svg')

p=ROOT/'version-concurrency.svg'
p.write_text('\n'.join(line.rstrip() for line in p.read_text().splitlines()).rstrip()+'\n')
