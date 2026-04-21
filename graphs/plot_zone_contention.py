#!/usr/bin/env python3
"""Zone Write Lock Contention — avg_wait vs avg_hold + per-group breakdown"""
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

threads = [1, 2, 4, 8, 16, 32, 64]
x = range(len(threads))

# Total zone wrlock
total_wait = [0.03, 0.41, 1.26, 4.85, 76.79, 397.57, 1075.67]
total_hold = [21.71, 22.44, 24.50, 32.50, 81.71, 102.37, 105.54]

# Per-group avg_wait (us)
iz_wait   = [0.03, 0.32, 0.98, 3.38, 46.13, 187.34, 424.59]
hot_wait  = [0.03, 0.43, 1.32, 5.23, 84.57, 470.64, 1200.46]
cold_wait = [0.03, 0.46, 1.38, 5.19, 84.07, 393.48, 1332.16]

# Per-group avg_hold (us)
iz_hold   = [22.03, 22.90, 25.13, 32.45, 73.29, 85.69, 87.43]
hot_hold  = [21.58, 22.25, 24.27, 32.74, 85.62, 109.20, 113.40]
cold_hold = [21.74, 22.51, 24.52, 31.92, 79.10, 99.30, 101.18]

# --- Figure 1: Total avg_wait vs avg_hold ---
fig1, ax1 = plt.subplots(figsize=(8, 5))
ax1.plot(x, total_wait, 'o-', color='#F44336', linewidth=2, markersize=7, label='avg_wait')
ax1.plot(x, total_hold, 's-', color='#2196F3', linewidth=2, markersize=7, label='avg_hold')
ax1.set_xticks(x)
ax1.set_xticklabels([str(t) for t in threads])
ax1.set_xlabel('Threads', fontsize=12)
ax1.set_ylabel('Time (us)', fontsize=12)
ax1.set_yscale('log')
ax1.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f'{v:g}'))
ax1.legend(fontsize=11)
ax1.set_title('Zone Write Lock — avg_wait vs avg_hold', fontsize=13)
ax1.grid(True, alpha=0.3, which='major')

fig1.tight_layout()
fig1.savefig('graphs/graph_zone_contention_total.png', dpi=150)
print('saved: graphs/graph_zone_contention_total.png')

# --- Figure 2: Per-group avg_wait comparison ---
fig2, ax2 = plt.subplots(figsize=(8, 5))
ax2.plot(x, iz_wait,   'o-', color='#4CAF50', linewidth=2, markersize=7, label='IZ (3 zones)')
ax2.plot(x, hot_wait,  's-', color='#F44336', linewidth=2, markersize=7, label='Hot (8 zones)')
ax2.plot(x, cold_wait, 'D-', color='#2196F3', linewidth=2, markersize=7, label='Cold (2 zones)')
ax2.set_xticks(x)
ax2.set_xticklabels([str(t) for t in threads])
ax2.set_xlabel('Threads', fontsize=12)
ax2.set_ylabel('avg_wait (us)', fontsize=12)
ax2.set_yscale('log')
ax2.yaxis.set_major_formatter(ticker.FuncFormatter(lambda v, _: f'{v:g}'))
ax2.legend(fontsize=11)
ax2.set_title('Zone Write Lock — avg_wait by Zone Group', fontsize=13)
ax2.grid(True, alpha=0.3, which='major')

fig2.tight_layout()
fig2.savefig('graphs/graph_zone_contention_groups.png', dpi=150)
print('saved: graphs/graph_zone_contention_groups.png')
