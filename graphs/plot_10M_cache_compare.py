#!/usr/bin/env python3
"""ZTree 10M Insert — Cache Size Comparison"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

threads = [1, 2, 4, 8, 16, 32, 64]

csmall  = [6245, 11400, 19763, 31251, 35355, 35265, 35442]
c1      = [7716, 13863, 23783, 35356, 36402, 36117, 36293]
ram     = [27356, 38858, 38843, 38900, 38950, 38900, 38984]

x = range(len(threads))

fig, ax = plt.subplots(figsize=(8, 5))
ax.plot(x, csmall, 'o-', color='#F44336', linewidth=2, markersize=7, label='16MB cache (~77%)')
ax.plot(x, c1,     's-', color='#2196F3', linewidth=2, markersize=7, label='64MB cache (~84%)')
ax.plot(x, ram,    'D-', color='#4CAF50', linewidth=2, markersize=7, label='RAM (100%)')

ax.set_xticks(x)
ax.set_xticklabels([str(t) for t in threads])
ax.set_xlabel('Threads', fontsize=12)
ax.set_ylabel('Throughput (ops/sec)', fontsize=12)
ax.set_ylim(0, 50000)
ax.set_yticks(range(0, 50001, 10000))
ax.yaxis.set_major_formatter(matplotlib.ticker.FuncFormatter(lambda v, _: f'{int(v/1000)}K'))
ax.legend(fontsize=10, loc='lower right')
ax.set_title('ZTree 10M Insert', fontsize=13)
ax.grid(True, alpha=0.3)

plt.tight_layout()
plt.savefig('logs/graph_10M_cache_compare.png', dpi=150)
print('saved: logs/graph_10M_cache_compare.png')
