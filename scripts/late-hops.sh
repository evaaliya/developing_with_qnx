#!/bin/bash
# For every late URGENT or COMMAND in the given host logs: its hops
# (source -> relay A, relay A -> relay B, relay B -> sink) from the sink
# line, the largest hop, and what the late check said about it (inside a
# fault of the check, inside a guest stall, or outside both).
#   scripts/late-hops.sh LOG...
# One line per late signal; a summary per verdict and largest hop last.
set -u

if [ "$#" -eq 0 ]; then
    echo "usage: scripts/late-hops.sh LOG..." >&2
    exit 2
fi
for log in "$@"; do
    awk -v log_name="$log" '
        # sink: signal N class C late, ..., hops a ns, b ns, c ns, total X ns
        /^sink: signal [0-9]+ class (URGENT|COMMAND) / && / late/ {
            n = $3
            for (i = 1; i <= NF; i++) if ($i == "hops") {
                h1[n] = $(i + 1) + 0; h2[n] = $(i + 3) + 0; h3[n] = $(i + 5) + 0
            }
            cls[n] = $5
        }
        # The number follows "late CLASS" with or without a label before it.
        /late-check: (FAIL )?late (URGENT|COMMAND) [0-9]+ / {
            for (i = 1; i < NF - 1; i++) if ($i == "late" && ($(i + 1) == "URGENT" || $(i + 1) == "COMMAND")) { k = $(i + 2); break }
            if ($0 ~ /FAIL late/) v[k] = "outside"
            else if ($0 ~ /inside the fault/) v[k] = "fault"
            else if ($0 ~ /inside a guest stall/) v[k] = "stall"
        }
        END {
            for (n in cls) {
                verdict = (n in v) ? v[n] : "unchecked"
                big = "source-relayA"; m = h1[n]
                if (h2[n] > m) { big = "relayA-relayB"; m = h2[n] }
                if (h3[n] > m) { big = "relayB-sink"; m = h3[n] }
                printf "late-hops: %s %s %s verdict %s hops %.1f %.1f %.1f ms largest %s\n", log_name, cls[n], n, verdict, h1[n] / 1e6, h2[n] / 1e6, h3[n] / 1e6, big
            }
        }' "$log"
done | sort > "${TMPDIR:-/tmp}/late-hops.$$"
cat "${TMPDIR:-/tmp}/late-hops.$$"
awk '{ c["verdict " $6 ", largest hop " $NF]++ }
    END { for (k in c) printf "late-hops: %s: %d\n", k, c[k] }' \
    "${TMPDIR:-/tmp}/late-hops.$$" | sort
rm -f "${TMPDIR:-/tmp}/late-hops.$$"
