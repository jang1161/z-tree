#!/usr/bin/env python3
"""Lock contention ratio by thread count (stacked bar)"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np

threads = ['1', '2', '4', '8', '16', '32', '64']
nlt     = [17.6, 15.2,  9.6,  3.7,  0.3,  0.1,  0.0]
zone    = [ 9.8, 51.2, 72.9, 86.8, 97.3, 98.3, 98.8]
node_rd = [59.2, 28.5, 14.5,  6.1,  1.2,  0.8,  0.7]
node_wr = [13.4,  5.1,  2.9,  3.5,  1.2,  1.0,  0.4]

x = np.arange(len(threads))
width = 0.55

fig, ax = plt.subplots(figsize=(8, 5))

b1 = ax.bar(x, zone,    width, label='Zone wrlock', color='#F44336')
b2 = ax.bar(x, node_rd, width, bottom=zone, label='Node rdlock', color='#2196F3')
b3 = ax.bar(x, node_wr, width, bottom=[z+r for z,r in zip(zone, node_rd)], label='Node wrlock', color='#FF9800')
b4 = ax.bar(x, nlt,     width, bottom=[z+r+w for z,r,w in zip(zone, node_rd, node_wr)], label='NLT wrlock', color='#4CAF50')

# Zone % label on each bar
for i, v in enumerate(zone):
    if v > 5:
        ax.text(i, v/2, f'{v:.0f}%', ha='center', va='center', fontsize=9, color='white', fontweight='bold')

ax.set_xticks(x)
ax.set_xticklabels(threads)
ax.set_xlabel('Threads', fontsize=12)
ax.set_ylabel('Share of Total Wait (%)', fontsize=12)
ax.set_ylim(0, 105)
ax.legend(fontsize=10, loc='upper right', bbox_to_anchor=(1.0, 0.85))
ax.set_title('Lock Contention Breakdown (10M insert, pwrite)', fontsize=13)
ax.grid(True, alpha=0.2, axis='y')

plt.tight_layout()
plt.savefig('graphs/graph_lock_ratio.png', dpi=150)
print('saved: graphs/graph_lock_ratio.png')
