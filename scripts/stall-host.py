"""Match guest stalls (stall-probe) with host samples (host-sample).

Reads DIR/clock-before.txt, DIR/clock-after.txt, DIR/host.txt and
DIR/probe.log written by scripts/stall-host.sh and prints:
  - the guest-to-host clock offset before and after (and the drift);
  - for stall size bands, how much CPU the busiest QEMU threads got
    during the stalls against all other periods, and how busy the cores
    were;
  - one line per stall over 20 ms.
A stall is the probe's [due, due + late] widened by one probe period
before it (the guest may have stopped any time after the last wake-up).
"""
import sys
from pathlib import Path


def offset(path):
    """Smallest host-minus-guest difference and the guest ms it was seen at."""
    best = None
    for line in Path(path).read_text().split("\n"):
        parts = line.split()
        if len(parts) != 2:
            continue
        diff = int(parts[0]) - int(parts[1])
        if best is None or diff < best[0]:
            best = (diff, int(parts[1]))
    return best


def main():
    d = Path(sys.argv[1])
    b0 = offset(d / "clock-before.txt")
    b1 = offset(d / "clock-after.txt")
    if b0 is None or b1 is None:
        print("stall-host: no clock pairs; cannot match")
        return 1
    off0, g0 = b0
    off1, g1 = b1
    print(f"stall-host: host minus guest clock {off0} ms before, {off1} ms after,"
          f" drift {off1 - off0} ms over {(g1 - g0) / 1000:.0f} s (interpolated per stall)")

    samples = []
    period = None
    for line in (d / "host.txt").read_text().split("\n"):
        parts = line.split()
        if line.startswith("host-sample:"):
            period = int(parts[parts.index("period") + 1])
            continue
        if not parts or parts[0] != "s" or "qemu" not in parts or "states" not in parts:
            continue  # the last line may be cut when the sampler is stopped
        t = int(parts[1])
        qi = parts.index("qemu")
        si = parts.index("states")
        cores = [int(x) for x in parts[3:qi]]
        # A thread cannot use more CPU than the period; a larger value is
        # a reused thread handle (fixed in host-sample.c since).
        threads = [float(x) if float(x) <= 1.5 * period else 0.0
                   for x in parts[qi + 1:si]]
        samples.append((t, cores, threads))
    if not samples:
        print("stall-host: no host samples")
        return 1
    nthreads = max(len(s[2]) for s in samples)
    totals = [0.0] * nthreads
    for _, _, th in samples:
        for i, v in enumerate(th):
            totals[i] += v
    order = sorted(range(nthreads), key=lambda i: -totals[i])
    vcpu = order[:2]
    main_t = order[2] if nthreads > 2 else None
    print(f"stall-host: {len(samples)} host samples of {period} ms; QEMU threads by"
          f" CPU used: " + ", ".join(f"#{i} {totals[i] / 1000:.1f} s" for i in order[:4])
          + f"; the two busiest (#{vcpu[0]}, #{vcpu[1]}) are taken as the vCPU"
          f" threads, #{main_t} as the main loop")

    stalls = []
    p0 = None
    for line in (d / "probe.log").read_text().split("\n"):
        parts = line.split()
        if line.startswith("stall-probe: start"):
            p0 = int(parts[2])
        if line.startswith("stall-probe: woke"):
            late_ms = int(parts[2]) / 1e6
            due = int(parts[6])
            stalls.append((due, late_ms))
    if p0 is None:
        print("stall-host: no probe start line")
        return 1
    t_first = samples[0][0]
    t_last = samples[-1][0]

    def host_off(guest_ms):
        # Linear between the offset before and the offset after.
        if g1 == g0:
            return off0
        return off0 + (off1 - off0) * (guest_ms - g0) / (g1 - g0)

    flagged = [False] * len(samples)
    rows = []
    for due, late in stalls:
        a = due - 10 + host_off(due)
        b = due + late + host_off(due)
        if a < t_first or b > t_last:
            continue
        idx = [i for i, s in enumerate(samples) if s[0] - period < b and s[0] > a]
        if not idx:
            continue
        for i in idx:
            flagged[i] = True
        span = len(idx) * period
        v = [sum(samples[i][2][k] if k < len(samples[i][2]) else 0 for i in idx) for k in vcpu]
        m = sum(samples[i][2][main_t] if main_t is not None and main_t < len(samples[i][2]) else 0 for i in idx)
        cores = [sum(samples[i][1][c] for i in idx) / len(idx) for c in range(len(samples[idx[0]][1]))]
        rows.append((due, late, span, v, m, cores))

    def share(vals, span):
        return 100.0 * vals / span if span else 0.0

    calm = [s for i, s in enumerate(samples) if not flagged[i]]
    if calm:
        span = len(calm) * period
        v = [sum(s[2][k] if k < len(s[2]) else 0 for s in calm) for k in vcpu]
        m = sum(s[2][main_t] if main_t is not None and main_t < len(s[2]) else 0 for s in calm)
        nc = len(calm[0][1])
        cores = [sum(s[1][c] for s in calm) / len(calm) for c in range(nc)]
        print(f"stall-host: periods without a stall ({len(calm)}): vCPU threads"
              f" {share(v[0], span):.0f} % and {share(v[1], span):.0f} % of one core,"
              f" main loop {share(m, span):.0f} %; cores busy "
              + " ".join(f"{c:.0f}" for c in cores))
    bands = [(5, 10), (10, 20), (20, 1e9)]
    for lo, hi in bands:
        sel = [r for r in rows if lo < r[1] <= hi]
        if not sel:
            print(f"stall-host: stalls {lo}-{hi if hi < 1e9 else 'inf'} ms: none in the sampled time")
            continue
        span = sum(r[2] for r in sel)
        v0 = sum(r[3][0] for r in sel)
        v1 = sum(r[3][1] for r in sel)
        m = sum(r[4] for r in sel)
        nc = len(sel[0][5])
        cores = [sum(r[5][c] for r in sel) / len(sel) for c in range(nc)]
        allbusy = sum(1 for r in sel if min(r[5]) >= 90)
        pbusy = sum(1 for r in sel if min(r[5][4:8] if nc >= 8 else r[5]) >= 90)
        print(f"stall-host: stalls {lo}-{hi if hi < 1e9 else 'inf'} ms ({len(sel)}):"
              f" vCPU threads {share(v0, span):.0f} % and {share(v1, span):.0f} %,"
              f" main loop {share(m, span):.0f} %; cores busy "
              + " ".join(f"{c:.0f}" for c in cores)
              + f"; all cores busy in {allbusy}, cores 4-7 all busy in {pbusy}")
    for due, late, span, v, m, cores in rows:
        if late > 20:
            print(f"stall-host: stall {late:.1f} ms at guest {due} ms: over {span} ms"
                  f" vCPU threads used {v[0]:.1f} and {v[1]:.1f} ms, main loop {m:.1f} ms;"
                  f" cores busy " + " ".join(f"{c:.0f}" for c in cores))
    return 0


if __name__ == "__main__":
    sys.exit(main())
