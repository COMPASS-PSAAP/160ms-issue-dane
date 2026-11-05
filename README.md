# 160 ms MPI stalls on Dane

## The problem

On Dane, an MPI exchange stalls for 160 ms when one rank receives small messages from more
than 64 different ranks on other nodes at the same time. A 0.6 ms exchange then takes about
161 ms. It happens in Open MPI and MVAPICH2, because both run over PSM2, the Omni-Path
messaging library (libpsm2 11.2.230 on Dane).

The cause is a fixed-size queue in libpsm2. For each message it receives, PSM2 sends a small
acknowledgment back to the sender. If the network card is busy at that moment, PSM2 puts the
acknowledgment in a queue to send later. That queue has 64 entries, one per sending peer
(`CTRL_MSG_QEUEUE_SIZE` in `ptl_ips/ips_proto.h`). When 65 or more peers need an
acknowledgment at once, the queue is full and PSM2 drops the extra acknowledgment without an
error. The sender gets no answer and waits for PSM2's error-check timer, whose default minimum
is 160 ms (`PSM2_ERRCHK_TIMEOUT=160:640:2`).

The trigger is the number of distinct senders; adding more messages or bytes per sender does
not cause it. All-to-all exchanges, dense halo exchanges, and incast patterns are affected once a
rank has more than 64 off-node senders. The stall appears only for eager messages, which are
those below PSM2's rendezvous threshold (64,000 bytes by default).

## Reproducing it

Build on a Dane login node:

```
module load openmpi
cmake -S . -B build && cmake --build build
```

Run on 2 nodes:

```
sbatch run.sbatch
```

The job runs `repro` with 64 senders per rank, then 65, then both again (256-byte messages,
up to 20,000 iterations each, stopping early after 100 stalls), first with the default settings
and then with fix 1 below. Output from the default part on two pbatch nodes:

```
M  64  iters  20000  p50    0.612  p99   12.468  max   19.123 ms  stalled    0  first     -1
M  65  iters   9842  p50    0.597  p99  160.151  max  160.850 ms  stalled  100  first     25  (stopped)
M  64  iters  20000  p50    0.592  p99    9.358  max   19.277 ms  stalled    0  first     -1
M  65  iters  11408  p50    0.598  p99   13.523  max  160.735 ms  stalled  100  first      5  (stopped)
```

With 64 senders, no iteration stalls. With 65, about 1 iteration in 100 takes 160 ms.

## The solution

We recommend fix 2, which removes the cause. Fix 1 is quicker to try because it needs no
rebuild.

### Fix 1: shorten the error-check timer

```
export PSM2_ERRCHK_TIMEOUT=1:640:2
```

Acknowledgments are still dropped, but each one now costs about 1 ms instead of 160 ms. In
`run.sbatch` the 65-sender runs should show no iterations over 80 ms. In our applications,
ordinary (non-stalled) iterations became 4 to 31% slower with this setting, and the slowdown
differed between exchange algorithms, so take care when using it to compare algorithms.

### Fix 2: rebuild libpsm2 with a larger queue

Build libpsm2 11.2.230, the version on Dane, with a 1024-entry queue:

```
curl -sSfL -o PSM2_11.2.230.tar.gz \
    https://github.com/cornelisnetworks/opa-psm2/archive/refs/tags/PSM2_11.2.230.tar.gz
md5sum PSM2_11.2.230.tar.gz        # 784255506ce8e319e8221cafa8b93229
mkdir opa-psm2 && tar xzf PSM2_11.2.230.tar.gz -C opa-psm2 --strip-components=1
cd opa-psm2
sed -i 's/^#define CTRL_MSG_QEUEUE_SIZE 64\b/#define CTRL_MSG_QEUEUE_SIZE 1024/' ptl_ips/ips_proto.h
grep -n 'define CTRL_MSG_QEUEUE_SIZE' ptl_ips/ips_proto.h    # must show 1024
make -j16                          # produces build_release/libpsm2.so.2
```

Use it per job, with no change to the system:

```
export LD_LIBRARY_PATH=/path/to/opa-psm2/build_release:$LD_LIBRARY_PATH
```

To check it with the reproducer: `PSM2_FIXED=/path/to/opa-psm2/build_release sbatch run.sbatch`.

On 4 nodes with 256-byte messages, the rebuilt library dropped no acknowledgments, where the
same build with the stock 64-entry queue dropped about 3 million per run. It had no stalls at 65
to 80 senders in 3,000 iterations and cut the median iteration at 128 senders from 161 ms to 1.1
to 1.6 ms. The fastest iterations were unchanged, so the larger queue has no measurable cost.

The fixes now uncover secondary effects: a smaller number of stalls still appears at high
sender counts, which we are still tracking down.

### Without changing PSM2

Keep the number of distinct off-node senders per rank at 64 or fewer, for example by
aggregating messages through one rank per node. Exchanges built this way did not stall.
