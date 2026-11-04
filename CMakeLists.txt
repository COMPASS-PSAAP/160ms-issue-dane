# On Dane, load Open MPI first (the default MPI module is MVAPICH2):
#   module load openmpi && cmake -S . -B build && cmake --build build   -> build/repro
cmake_minimum_required(VERSION 3.16)
project(psm2_160ms C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(MPI REQUIRED COMPONENTS C)
add_executable(repro repro.c)
target_link_libraries(repro PRIVATE MPI::MPI_C)
set_property(TARGET repro PROPERTY C_STANDARD 99)
