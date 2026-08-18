"""Aggregate fyai-prof tables from every process in one run."""
import sys

rows = {}
procs = []
cur = None
for line in open(sys.argv[1], errors="replace"):
    if "[fyai-prof]" not in line:
        continue
    f = line.split("[fyai-prof]", 1)[1].split()
    if not f:
        continue
    if f[0] == "phase":
        cur = {}
        procs.append(cur)
        continue
    if len(f) != 6 or cur is None:
        continue
    name, n, total, avg, mn, mx = f
    cur[name] = (int(n), float(total), float(mx))
    r = rows.setdefault(name, [0, 0.0, 0.0])
    r[0] += int(n)
    r[1] += float(total)
    r[2] = max(r[2], float(mx))

att = [p.get("publish_attempt", (0,))[0] for p in procs]
lost = [p.get("publish_cas_lost", (0,))[0] for p in procs]
pub = sum(1 for a in att if a)
print("processes=%d  publishes=%d  attempts=%d  lost=%d  "
      "attempts/publish=%.1f  worst single publish=%d attempts (limit 64)"
      % (len(procs), pub, sum(att), sum(lost),
         (sum(att) / pub) if pub else 0, max(att) if att else 0))
print("%-24s %8s %12s %12s %12s" % ("phase", "n", "total_ms", "avg_ms", "max_ms"))
for name in ("publish_total", "commit_durable", "commit_build_branches",
             "commit_cas_won", "commit_cas_lost", "publish_reconcile", "agent_input_internalize"):
    if name not in rows:
        continue
    n, total, mx = rows[name]
    print("%-24s %8d %12.3f %12.4f %12.3f" %
          (name, n, total, (total / n) if n else 0.0, mx))
