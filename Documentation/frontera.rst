.. _compile_run_frontera:


Frontera
========


Add these lines to ``~/.bashrc``:

::

    module switch python3 python3/3.9.2
    module use /work2/09160/ulrich/frontera/spack/share/spack/modules/linux-centos7-cascadelake
    module load seissol-env-develop-intel-19.1.1.217-x52n3zf
    export CC=mpiicc
    export CXX=mpiicpc
    export FC=mpiifort

This will load a preinstalled seissol-env module.

Alternatively (and for reference), to compile seissol-env on Frontera, follow the procedure below:

.. code-block:: bash

    git clone --depth 1 --branch v0.18.1 https://github.com/spack/spack.git
    cd spack
    echo "export SPACK_ROOT=$PWD" >> $HOME/.bashrc
    echo "export PATH=\$SPACK_ROOT/bin:\$PATH" >> $HOME/.bashrc
    # clone seissol-spack-aid and add the repository
    git clone --branch supermuc_NG https://github.com/SeisSol/seissol-spack-aid.git
    cd seissol-spack-aid
    spack repo add ./spack
    spack compiler find

Following the workaround proposed in https://github.com/spack/spack/issues/10308, precise the module of the intel compilers in ``~/.spack/linux/compilers.yaml`` by changing ``modules: []`` to ``modules: ['intel/19.1.1']``.

Then, update ``~/.spack/packages.yaml`` as follow:

.. code-block:: yaml

    packages:
      autoconf:
        externals:
        - spec: autoconf@2.69
          prefix: /opt/apps/autotools/1.2
      python:
        externals:
        - spec: python@3.9.2
          buildable: false
          prefix: /opt/apps/intel19/python3/3.9.2
          modules:
          - python3/3.9.2
      intel-mpi:
        buildable: false
        externals:
        - spec: intel-mpi@2019.0.9
          modules:
          - impi/19.0.9
      all:
        providers:
          mpi: [intel-mpi]

(note that the compilation was not successful with trying to add the cmake/3.24.2 module to packages.yaml).

Finally, install seissol-env with 

.. code-block:: bash

    spack install -j 16 seissol-env %intel@19.1.1.217 ^intel-mpi

and create a module with:

.. code-block:: bash

    spack module tcl refresh seissol-env

To access the module at start up, add to your ``~/.bashrc``:

.. code-block:: bash

    module use $SPACK_ROOT/share/spack/modules/linux-centos7-cascadelake/

Finally, install SeisSol with cmake, as usual, with ``-DHOST_ARCH=skx``.


For large runs, it is recommanded to have executable, dynamic libraries, setup and outputs on scratch.
That is, WORK is not built for large, intensive IOs, and loading the shared libraries from 8000+ nodes at once is quite intensive.
This could potentially break the filesystem.
The dynamic libraries can be copied to $SCRATCH with the following commands:

.. code-block:: bash

    cd $SCRATCH
    mkdir lib_dump && cd lib_dump
    # replace by the path to your seissol executable
    seissol_exe=../SeisSol/build/SeisSol_Release_dskx_6_viscoelastic2
    # cp to lib_dump all shared libraries from the spack environment that seissol depends on:
    ldd $(seissol_exe) | grep spack | awk '{print $(NF-1)}' | while read -r lib; do cp "$lib" ./; done
    # manually add libimpalajit.so, whose path was not listed in seissol_exe:
    cp /work2/09160/ulrich/frontera/spack/opt/spack/linux-centos7-cascadelake/intel-19.1.1.217/impalajit-main-rdjaykqjjbb645iny6nexrtnup27ejpg/lib64/libimpalajit.so /scratch1/09160/ulrich/lib_dump/

Then you can update your ~/.bashrc and unload the seissol-env module, e.g. with:


.. code-block:: bash

    echo "export LD_LIBRARY_PATH=$SCRATCH/lib_dump/:\$LD_LIBRARY_PATH" >> ~/.bashrc
    seissol_env_module=$(module list 2>&1 | awk '/seissol-env/{print $NF}')
    module unload $seissol_env_module

Finally, we provide an example of launch script used for running a full-machine frontera run.
In particular, note how timeout and retry count are increased.

.. code-block:: bash

    #!/bin/bash
    #SBATCH --chdir=./
    #SBATCH -o ./%j.out       # Name of stdout output file
    #SBATCH -e ./%j.out       # Name of stderr error file
    #SBATCH -p debug         # Queue (partition) name
    #SBATCH --nodes=8192
    #SBATCH --ntasks-per-node=2
    #SBATCH -t 24:00:00        # Run time (hh:mm:ss)
    #SBATCH -A EAR22007       # Project/Allocation name (req'd if you have more than 1)

    # Any other commands must follow all #SBATCH directives...
    module list
    pwd
    date

    #Prevents errors such as experience in Issue #691
    export I_MPI_SHM_HEAP_VSIZE=32768

    export OMP_NUM_THREADS=27
    export OMP_PLACES="cores(27)"
    #export OMP_PROC_BIND="close"

    export XDMFWRITER_ALIGNMENT=8388608
    export XDMFWRITER_BLOCK_SIZE=8388608
    export ASYNC_MODE=THREAD
    export ASYNC_BUFFER_ALIGNMENT=8388608

    echo 'num_nodes:' $SLURM_JOB_NUM_NODES 'ntasks:' $SLURM_NTASKS
    ulimit -Ss 2097152

    source ~cazes/texascale_settings.sh
    export UCX_TLS=knem,dc
    export UCX_DC_MLX5_TIMEOUT=35000000.00us
    export UCX_DC_MLX5_RNR_TIMEOUT=35000000.00us
    export UCX_DC_MLX5_RETRY_COUNT=180
    export UCX_DC_MLX5_RNR_RETRY_COUNT=180
    export UCX_RC_MLX5_TIMEOUT=35000000.00us
    export UCX_RC_MLX5_RNR_TIMEOUT=35000000.00us
    export UCX_RC_MLX5_RETRY_COUNT=180
    export UCX_RC_MLX5_RNR_RETRY_COUNT=180
    export UCX_UD_MLX5_TIMEOUT=35000000.00us
    export UCX_UD_MLX5_RETRY_COUNT=180


    # Launch MPI code... 
    seissol_exe=SeisSol_Release_dskx_6_viscoelastic2
    echo $seissol_exe
    time -p ibrun $seissol_exe parameters.par
