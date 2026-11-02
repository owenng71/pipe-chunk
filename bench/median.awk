# median.awk -v keys=N RAW.csv — one row per configuration from a one-row-per-run CSV.
# The first N columns are the configuration key and column N+1 is the run number; every later
# column is a metric. Prints the key, the run count, the median of each metric and, if there is a
# throughput_mb_s column, its min and max. Rows keep the order in which each key first appears.
# A metric that is "n/a" (or empty) in any run is "n/a" in the median. Run with LC_ALL=C.
BEGIN { FS = OFS = "," }

# Median of the values stored for (key, col); also sets globals lo and hi.
function median(key, col,    n, i, j, t, v, s) {
    n = cnt[key]
    lo = hi = "n/a"
    for (i = 1; i <= n; i++) {
        s[i] = val[key, col, i]
        if (s[i] == "n/a" || s[i] == "") return "n/a"
        v[i] = s[i] + 0
    }
    for (i = 2; i <= n; i++)            # insertion sort of values and their original strings
        for (j = i; j > 1 && v[j - 1] > v[j]; j--) {
            t = v[j]; v[j] = v[j - 1]; v[j - 1] = t
            t = s[j]; s[j] = s[j - 1]; s[j - 1] = t
        }
    lo = s[1]; hi = s[n]
    if (n % 2) return s[(n + 1) / 2]    # the run's own text: no reformatting of big integers
    t = (v[n / 2] + v[n / 2 + 1]) / 2
    return t == int(t) ? sprintf("%d", t) : sprintf("%.6g", t)
}

NR == 1 {
    line = ""
    for (c = 1; c <= keys; c++) line = line $c OFS
    line = line "runs"
    for (c = keys + 2; c <= NF; c++) {
        line = line OFS $c
        if ($c == "throughput_mb_s") tcol = c
    }
    if (tcol) line = line OFS "throughput_min_mb_s" OFS "throughput_max_mb_s"
    print line
    last = NF
    next
}
{
    key = $1
    for (c = 2; c <= keys; c++) key = key OFS $c
    if (!(key in cnt)) order[++nkeys] = key
    n = ++cnt[key]
    for (c = keys + 2; c <= NF; c++) val[key, c, n] = $c
}
END {
    for (k = 1; k <= nkeys; k++) {
        key = order[k]
        line = key OFS cnt[key]
        for (c = keys + 2; c <= last; c++) line = line OFS median(key, c)
        if (tcol) { median(key, tcol); line = line OFS lo OFS hi }
        print line
    }
}
