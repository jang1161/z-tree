#!/usr/bin/env python3
"""fio 4KB Write — Zone-Isolated Scaling"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

jobs = [1, 2, 4, 8, 16]
bw   = [179, 203, 197, 195, 195]
kops = [45.8, 52.0, 50.4, 49.9, 49.9]

x = range(len(jobs))

fig, ax1 = plt.subplots(figsize=(8, 5))

l1, = ax1.plot(x, bw, 'o-', color='#2196F3', linewidth=2, markersize=7, label='Total BW (MiB/s)')
for i, v in enumerate(bw):
    ax1.annotate(f'{v}', xy=(i, v), xytext=(0, 10), textcoords='offset points',
                 ha='center', fontsize=9, color='#2196F3')
ax1.set_xticks(x)
ax1.set_xticklabels([str(j) for j in jobs])
ax1.set_xlabel('Number of Zones', fontsize=12)
ax1.set_ylabel('Total BW (MiB/s)', fontsize=12, color='#2196F3')
ax1.set_ylim(0, 280)
ax1.tick_params(axis='y', labelcolor='#2196F3')

ax2 = ax1.twinx()
l2, = ax2.plot(x, kops, 's--', color='#F44336', linewidth=2, markersize=7, label='Total IOPS (K)')
for i, v in enumerate(kops):
    ax2.annotate(f'{v}', xy=(i, v), xytext=(0, -15), textcoords='offset points',
                 ha='center', fontsize=9, color='#F44336')
ax2.set_ylabel('Total IOPS (K)', fontsize=12, color='#F44336')
ax2.set_ylim(0, 70)
ax2.tick_params(axis='y', labelcolor='#F44336')

ax1.legend([l1, l2], ['Total BW (MiB/s)', 'Total IOPS (K)'], fontsize=10, loc='center right')
ax1.set_title('fio 4KB Write — Zone-Isolated Scaling (psync, iodepth=1)', fontsize=12)
ax1.grid(True, alpha=0.3)

plt.tight_layout()
plt.savefig('graphs/graph_fio_isolated_scaling.png', dpi=150)
print('saved: graphs/graph_fio_isolated_scaling.png')
