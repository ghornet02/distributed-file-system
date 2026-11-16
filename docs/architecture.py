"""Render docs/architecture.png.

Usage: python3 docs/architecture.py docs/architecture
"""
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch

out = sys.argv[1]

INK = "#1e293b"
MUTED = "#64748b"
BLUE, BLUE_BG = "#0369a1", "#e0f2fe"
AMBER, AMBER_BG = "#b45309", "#fef3c7"
GREEN, GREEN_BG = "#15803d", "#dcfce7"
VIOLET = "#7c3aed"

plt.rcParams["font.family"] = "DejaVu Sans"
fig, ax = plt.subplots(figsize=(14, 7.6))
ax.set_xlim(0, 140)
ax.set_ylim(0, 76)
ax.axis("off")
fig.patch.set_facecolor("white")


def box(x, y, w, h, fc, ec, lw=1.6, ls="-", r=1.6, z=2):
    ax.add_patch(FancyBboxPatch((x, y), w, h, boxstyle=f"round,pad=0,rounding_size={r}",
                                fc=fc, ec=ec, lw=lw, ls=ls, zorder=z))


def arrow(p, q, color, ls="-", lw=2.0, rad=0.0):
    ax.add_patch(FancyArrowPatch(p, q, arrowstyle="-|>", mutation_scale=16, color=color, lw=lw,
                                 ls=ls, connectionstyle=f"arc3,rad={rad}", zorder=1,
                                 shrinkA=0, shrinkB=0))


def label(x, y, text, color, size=10.5, rot=0, weight="normal"):
    ax.text(x, y, text, color=color, fontsize=size, ha="center", va="center", rotation=rot,
            rotation_mode="anchor", weight=weight, zorder=5,
            bbox=dict(fc="white", ec="none", pad=1.5))


ax.text(70, 73, "Distributed File System — Architecture", ha="center", va="center",
        fontsize=16, weight="bold", color=INK)

nx, ny, nw, nh = 52, 40, 36, 26
box(nx, ny, nw, nh, AMBER_BG, AMBER, lw=2)
ax.text(nx + nw / 2, ny + nh - 4, "Name Server", ha="center", fontsize=15, weight="bold", color=INK)
for i, line in enumerate(["file table: name · owner · location",
                          "storage-server registry",
                          "heartbeat monitor · load balancer",
                          "owner permission checks"]):
    ax.text(nx + nw / 2, ny + nh - 9.5 - i * 4, line, ha="center", fontsize=10.5, color="#78350f")

cx, cy, cw, ch = 4, 4, 26, 30
box(cx, cy, cw, ch, "none", "#94a3b8", lw=1.3, ls=(0, (5, 4)), z=1)
ax.text(cx + cw / 2, cy + ch - 3.2, "Clients", ha="center", fontsize=12.5, weight="bold", color=INK)
for i, user in enumerate(["alice", "bob"]):
    y = cy + ch - 14 - i * 11
    box(cx + 3, y, cw - 6, 8, BLUE_BG, BLUE)
    ax.text(cx + cw / 2, y + 4, f"Client ({user})", ha="center", va="center", fontsize=11.5, color=INK)

sx, sy, sw, sh = 106, 4, 30, 38
box(sx, sy, sw, sh, "none", "#94a3b8", lw=1.3, ls=(0, (5, 4)), z=1)
ax.text(sx + sw / 2, sy + sh - 3.2, "Storage Servers", ha="center", fontsize=12.5, weight="bold", color=INK)
for i, (name, dashed) in enumerate([("Storage Server 1", False), ("Storage Server 2", False),
                                    ("Storage Server N", True)]):
    y = sy + sh - 15 - i * 10.5
    box(sx + 3, y, sw - 6, 8.5, GREEN_BG, GREEN, ls=(0, (4, 3)) if dashed else "-")
    ax.text(sx + sw / 2, y + 5.6, name, ha="center", va="center", fontsize=11, color=INK)
    ax.text(sx + sw / 2, y + 2.4, "…" if dashed else "local disk · flock()", ha="center",
            va="center", fontsize=9, color=MUTED)

p1, q1 = (cx + cw + 1, 31), (nx - 1, 53)
p2, q2 = (nx - 1, 45), (cx + cw + 1, 23)
arrow(p1, q1, BLUE)
label(31, 47, "① request", BLUE, size=11)
arrow(p2, q2, AMBER, ls=(0, (6, 3)))
label(48, 27, "② reply, or\nSS <ip> <port> redirect", AMBER, size=10.5)

p3, q3 = (nx + nw + 1, 53), (sx - 1, 36)
p4, q4 = (sx - 1, 28), (nx + nw + 1, 45)
arrow(p3, q3, AMBER)
label(106, 50, "CREATE · DELETE", AMBER, size=10.5)
arrow(p4, q4, GREEN, ls=(0, (1.5, 2.5)), lw=2.2)
label(87, 27, "heartbeat\nREGISTER_SS every 5 s", GREEN, size=10.5)

arrow((cx + cw, 10), (sx, 10), VIOLET, lw=2.6)
label(68, 10, "③ READ · WRITE — direct data path", VIOLET, size=11.5, weight="bold")

ax.text(70, 1.2, "Metadata goes through the Name Server; file data flows directly between clients and storage servers.",
        ha="center", fontsize=10, color=MUTED, style="italic")

fig.savefig(out + ".png", dpi=170, bbox_inches="tight", facecolor="white")
