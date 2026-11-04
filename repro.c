/* repro.c: shows the 160 ms PSM2 stall on Dane with 65 senders and not with 64.

Every iteration, every rank receives one BYTES-byte message from each of M different ranks
on the other node and sends one to M ranks there (post receives, post sends, wait for all).
With M <= 64 no iteration stalls. With M >= 65, PSM2's 64-entry acknowledgment queue can
overflow, and an iteration takes about 160 ms longer (see README.md).

Partners: rank (node n, index l) receives from (other node, index l+t) and sends to (other
node, index l-t), t = 0 .. M-1, modulo the ranks per node. Needs exactly 2 nodes with the same
number of ranks each, ranks in node blocks (Slurm's default), and M <= ranks per node.

Output (rank 0): one line per M with the iterations timed, the median, p99, and max of the
slowest rank's time per iteration, the number of iterations over 80 ms (stalled), and the
first stalled iteration. A run stops after MAXSTALL stalled iterations. The first WARM
iterations are not timed.

Run:  srun -N 2 --ntasks-per-node=112 ./build/repro ITERS M[,M...] [BYTES, default 256]
*/
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WARM 10
#define MAXSTALL 100
#define STALL_S 0.080

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    int me, np;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &me);
    MPI_Comm_size(MPI_COMM_WORLD, &np);
    if (argc < 3 || atoi(argv[1]) < 1) {
        if (!me) fprintf(stderr, "usage: %s ITERS M[,M...] [BYTES]\n", argv[0]);
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    int iters = atoi(argv[1]);
    size_t bytes = argc > 3 ? strtoul(argv[3], NULL, 10) : 256;

    /* Ranks sharing a node must be exactly [0, ppn) or [ppn, 2 ppn). */
    MPI_Comm node;
    int lrank, lsize, ppn = np / 2;
    MPI_Comm_split_type(MPI_COMM_WORLD, MPI_COMM_TYPE_SHARED, 0, MPI_INFO_NULL, &node);
    MPI_Comm_rank(node, &lrank);
    MPI_Comm_size(node, &lsize);
    int bad = np % 2 || lsize != ppn || lrank != me % ppn;
    MPI_Allreduce(MPI_IN_PLACE, &bad, 1, MPI_INT, MPI_LOR, MPI_COMM_WORLD);
    if (bad) {
        if (!me) fprintf(stderr, "need 2 nodes, equal ranks per node, ranks in node blocks\n");
        MPI_Abort(MPI_COMM_WORLD, 1);
    }
    int l = me % ppn, other = me < ppn ? ppn : 0;

    char host[MPI_MAX_PROCESSOR_NAME], *hosts = malloc((size_t)np * MPI_MAX_PROCESSOR_NAME);
    int len;
    memset(host, 0, sizeof host);
    MPI_Get_processor_name(host, &len);
    MPI_Gather(host, MPI_MAX_PROCESSOR_NAME, MPI_CHAR, hosts, MPI_MAX_PROCESSOR_NAME, MPI_CHAR, 0,
               MPI_COMM_WORLD);
    if (!me) {
        char lib[MPI_MAX_LIBRARY_VERSION_STRING];
        const char *timer = getenv("PSM2_ERRCHK_TIMEOUT");
        MPI_Get_library_version(lib, &len);
        lib[strcspn(lib, "\n")] = 0;
        printf("# %s\n# PSM2_ERRCHK_TIMEOUT %s\n# nodes %s %s, %d ranks each; %d iterations, %zu B\n",
               lib, timer ? timer : "unset (160:640:2)", hosts, hosts + (size_t)ppn * MPI_MAX_PROCESSOR_NAME,
               ppn, iters, bytes);
        fflush(stdout);
    }

    char *sbuf = malloc(bytes ? bytes : 1), *rbuf = malloc((size_t)ppn * (bytes ? bytes : 1));
    MPI_Request *req = malloc(2 * (size_t)ppn * sizeof *req);
    double *times = malloc((size_t)iters * sizeof *times);
    memset(sbuf, me & 0xff, bytes ? bytes : 1);

    for (char *tok = strtok(argv[2], ","); tok; tok = strtok(NULL, ",")) {
        int m = atoi(tok), n = 0, nstall = 0, first = -1;
        if (m < 1 || m > ppn) {
            if (!me) printf("M %d skipped: must be 1 .. %d\n", m, ppn);
            continue;
        }
        for (int it = -WARM; it < iters; it++) {
            MPI_Barrier(MPI_COMM_WORLD);
            double dt = MPI_Wtime();
            for (int t = 0; t < m; t++)
                MPI_Irecv(rbuf + (size_t)t * bytes, (int)bytes, MPI_BYTE, other + (l + t) % ppn, 0,
                          MPI_COMM_WORLD, &req[t]);
            for (int t = 0; t < m; t++)
                MPI_Isend(sbuf, (int)bytes, MPI_BYTE, other + (l - t + ppn) % ppn, 0, MPI_COMM_WORLD,
                          &req[m + t]);
            MPI_Waitall(2 * m, req, MPI_STATUSES_IGNORE);
            dt = MPI_Wtime() - dt;
            /* Every rank gets the same max, so every rank stops at the same iteration. */
            MPI_Allreduce(MPI_IN_PLACE, &dt, 1, MPI_DOUBLE, MPI_MAX, MPI_COMM_WORLD);
            if (it < 0)
                continue;
            times[n++] = dt;
            if (dt > STALL_S) {
                if (first < 0) first = it;
                if (++nstall == MAXSTALL) break;
            }
        }
        if (!me) {
            qsort(times, n, sizeof *times, cmp);
            printf("M %3d  iters %6d  p50 %8.3f  p99 %8.3f  max %8.3f ms  stalled %4d  first %6d%s\n",
                   m, n, 1e3 * times[n / 2], 1e3 * times[n * 99 / 100],
                   1e3 * times[n - 1], nstall, first, nstall == MAXSTALL ? "  (stopped)" : "");
            fflush(stdout);
        }
    }
    MPI_Finalize();
    return 0;
}
