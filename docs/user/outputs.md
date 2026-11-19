# Outputs

Simulations report results through three channels: human-readable
console text, exact CSV logs, and live shared-memory/socket streams. The
CSV examples below were generated exactly as shown while writing this
guide.

## Console output

The stepped-sim console sink prints one header block plus one line per
particle at the final step:

```console
$ ./build/PhysicsSimApp sim run --steps 100 --particles 1
sim run: solver=projectile g=9.81
steps: 100 x 0.01s -> t=1s
particles: 1
p0: x=10 y=5.04595 vx=10 vy=0.19
```

Signal runs print one line per source instead (`s0: sine value=... U`),
and a `csv: <path> (N rows)` trailer names each file sink's row count.
Console text is for humans; anything you need to re-read belongs in a
CSV log.

## Snapshot CSV (`sim run --emit-csv`)

`--emit-csv <path>` records the full snapshot stream — one row per
particle per emitted step:

```console
$ ./build/PhysicsSimApp sim run --steps 5 --particles 1 --emit-csv /tmp/opencode/sim-example.csv
sim run: solver=projectile g=9.81
steps: 5 x 0.01s -> t=0.05s
particles: 1
p0: x=0.5 y=0.485285 vx=10 vy=9.5095
csv: /tmp/opencode/sim-example.csv (5 rows)
$ cat /tmp/opencode/sim-example.csv
step,time,slot,entity_index,entity_generation,x,y,vx,vy
1,0.01,0,0,0,0.10000000000000001,0.099018999999999996,10,9.9018999999999995
2,0.02,0,0,0,0.20000000000000001,0.19705699999999998,10,9.803799999999999
3,0.029999999999999999,0,0,0,0.30000000000000004,0.29411399999999999,10,9.7056999999999984
4,0.040000000000000001,0,0,0,0.40000000000000002,0.39018999999999998,10,9.6075999999999979
5,0.050000000000000003,0,0,0,0.5,0.48528499999999997,10,9.5094999999999974
```

Columns are `step,time,slot,entity_index,entity_generation,x,y,vx,vy`;
the index/generation pair is the particle's stable identity across the
run. Values print with `max_digits10` precision, so a CSV value parses
back to the exact same double (round-trip exact). The same file replays
onto the shared-memory bus with the CSV-to-bus bridge tool.

## Signals CSV (`sig run --emit-csv`)

Signal runs log one row per source per emitted step. Columns are
`step,time,sig_id,value,quantity`, where `sig_id` is the source's stable
serial (never reused within a run) and `quantity` is `U` or `I`:

```console
$ ./build/PhysicsSimApp sig run --sig-kind sine --sig-freq 1kHz --sig-amp 3.3V --steps 10 --dt 0.1ms --emit-csv /tmp/opencode/sig-example.csv
sig run: kind=sine
steps: 10 x 0.0001s -> t=0.001s
signals: 1
s0: sine value=2.12272e-15 U
csv: /tmp/opencode/sig-example.csv (10 rows)
$ cat /tmp/opencode/sig-example.csv
step,time,sig_id,value,quantity
1,0.0001,0,1.9396913325651612,U
2,0.00020000000000000001,0,3.1384865037740064,U
3,0.00030000000000000003,0,3.1384865037740068,U
4,0.00040000000000000002,0,1.9396913325651617,U
5,0.00050000000000000001,0,4.0413344371862655e-16,U
6,0.00060000000000000006,0,-1.9396913325651608,U
7,0.0007000000000000001,0,-3.1384865037740064,U
8,0.00080000000000000015,0,-3.138486503774006,U
9,0.00090000000000000019,0,-1.9396913325651595,U
10,0.0010000000000000002,0,2.1227218975731601e-15,U
```

The same `max_digits10` exact-round-trip guarantee applies: parsed
values compare `==` to the in-simulation doubles. Decimation
(`--emit-every`, per-output `--emit-csv-stride`) and subscriptions
(`--emit-csv-select`) change which rows land here; keep the recorder at
full rate and decimate only live outputs, so the log stays complete
while displays stay cheap.

## CSV is replay, not a checkpoint

A snapshot CSV is an output log: it replays observations (onto the bus,
into the GUI's CSV-replay source, or into your own analysis). It is
never a restart checkpoint — reloading it does not restore RNG state,
serials, or script stores. The authoritative restart path is the
versioned binary `save_state`/`load_state` mechanism (engine-level; no
CLI flag needed to know the principle: logs observe, checkpoints
resume).

## Diagnosing failures

A zero exit with a `csv:` trailer means the log is complete. Non-zero
exits point at [Troubleshooting](troubleshooting.md): exit 2 for bad
arguments (nothing was written or the file holds only valid rows),
exit 1 for runtime/check failures (a `FAIL` summary names the cause).

## Per-domain fields

What each domain's rows and channels mean beyond the column shapes above
— particle identity, signal serials and quantities, sequential channels —
is documented in [domain outputs](../domain-outputs.md) and not repeated
here.
