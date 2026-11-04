#!/bin/bash
#SBATCH -J psm2-160ms
#SBATCH -p pbatch
#SBATCH -N 2
#SBATCH --ntasks-per-node=112
#SBATCH -t 0:20:00
# Submit from this directory after building (see README.md):
#   sbatch run.sbatch
# To also test the rebuilt libpsm2 (README.md, fix 2), pass the directory holding its libpsm2.so.2:
#   PSM2_FIXED=/path/to/opa-psm2/build_release sbatch run.sbatch
# Output goes to slurm-<jobid>.out in this directory.
module load openmpi || exit 1
# Open MPI over PSM2 directly; fail rather than fall back to another transport.
export OMPI_MCA_pml=cm OMPI_MCA_mtl=psm2
unset PSM2_ERRCHK_TIMEOUT
ARGS="20000 64,65,64,65 256"

echo "== stock libpsm2, default timer"
srun ./build/repro $ARGS

echo "== fix 1: PSM2_ERRCHK_TIMEOUT=1:640:2"
PSM2_ERRCHK_TIMEOUT=1:640:2 srun ./build/repro $ARGS

if [ -n "$PSM2_FIXED" ]; then
    echo "== fix 2: libpsm2 with a 1024-entry queue from $PSM2_FIXED"
    LD_LIBRARY_PATH=$PSM2_FIXED:$LD_LIBRARY_PATH srun ./build/repro $ARGS
fi
