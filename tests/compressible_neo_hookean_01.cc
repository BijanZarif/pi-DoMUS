#include "n_fields_problem.h"
#include "interfaces/compressible_neo_hookean.h"
#include "tests.h"

using namespace dealii;
int main (int argc, char *argv[])
{

  Utilities::MPI::MPI_InitFinalize mpi_initialization(argc, argv,
                                                      numbers::invalid_unsigned_int);

  initlog();
  deallog.threshold_double(1.e-2);

  const int dim = 3;
  const int spacedim = 3;

  CompressibleNeoHookeanInterface<dim,spacedim> energy;
  NFieldsProblem<dim,spacedim,dim> n_problem (energy);
  ParameterAcceptor::initialize(SOURCE_DIR "/parameters/compressible_neo_hookean_01.prm", "used_parameters.prm");


  n_problem.run ();

  return 0;
}
