#include "solution.h"
#include "navier_stokes.h"


using namespace dealii;

  template <int dim>
  NavierStokes<dim>::Parameters::Parameters (const std::string &parameter_filename)
    :
    end_time (1e8),
    initial_global_refinement (2),
    //step_global_refinement (1),
    initial_adaptive_refinement (2),
    adaptive_refinement_interval (10),
    navier_stokes_velocity_degree (2),
    use_locally_conservative_discretization (true)
  {
    ParameterHandler prm;
    NavierStokes<dim>::Parameters::declare_parameters (prm);

    std::ifstream parameter_file (parameter_filename.c_str());

    if (!parameter_file)
      {
        parameter_file.close ();

        std::ostringstream message;
        message << "Input parameter file <"
                << parameter_filename << "> not found. Creating a"
                << std::endl
                << "template file of the same name."
                << std::endl;

        std::ofstream parameter_out (parameter_filename.c_str());
        prm.print_parameters (parameter_out,
                              ParameterHandler::Text);

        AssertThrow (false, ExcMessage (message.str().c_str()));
      }

    const bool success = prm.read_input (parameter_file);
    AssertThrow (success, ExcMessage ("Invalid input parameter file."));

    parse_parameters (prm);
  }


  template <int dim>
  void
  NavierStokes<dim>::Parameters::
  declare_parameters (ParameterHandler &prm)
  {
    prm.declare_entry ("End time", "1e8",
                       Patterns::Double (0),
                       "The end time of the simulation in seconds.");
    prm.declare_entry ("Initial global refinement", "2",
                       Patterns::Integer (0),
                       "The number of global refinement steps performed on "
                       "the initial coarse mesh, before the problem is first "
                       "solved there.");
    /*prm.declare_entry ("Step global refinement", "2",
                       Patterns::Integer (0),
                       "The number of global refinement steps performed on "
                       "the initial coarse mesh, before the problem is first "
                       "solved there.");*/
    prm.declare_entry ("Initial adaptive refinement", "2",
                       Patterns::Integer (0),
                       "The number of adaptive refinement steps performed after "
                       "initial global refinement.");
    prm.declare_entry ("Time steps between mesh refinement", "10",
                       Patterns::Integer (1),
                       "The number of time steps after which the mesh is to be "
                       "adapted based on computed error indicators.");
    prm.declare_entry ("Generate graphical output", "false",
                       Patterns::Bool (),
                       "Whether graphical output is to be generated or not. "
                       "You may not want to get graphical output if the number "
                       "of processors is large.");
    prm.declare_entry ("Time steps between graphical output", "50",
                       Patterns::Integer (1),
                       "The number of time steps between each generation of "
                       "graphical output files.");

    prm.enter_subsection ("Discretization");
    {
      prm.declare_entry ("Navier Stokes velocity polynomial degree", "2",
                         Patterns::Integer (1),
                         "The polynomial degree to use for the velocity variables "
                         "in the Navier Stokes system.");
      prm.declare_entry ("Use locally conservative discretization", "true",
                         Patterns::Bool (),
                         "Whether to use a Navier Stokes discretization that is locally "
                         "conservative at the expense of a larger number of degrees "
                         "of freedom, or to go with a cheaper discretization "
                         "that does not locally conserve mass (although it is "
                         "globally conservative.");
    }
    prm.leave_subsection ();
  }

  template <int dim>
  void
  NavierStokes<dim>::Parameters::
  parse_parameters (ParameterHandler &prm)
  {
    end_time                    = prm.get_double ("End time");
    initial_global_refinement   = prm.get_integer ("Initial global refinement");
    initial_adaptive_refinement = prm.get_integer ("Initial adaptive refinement");

    adaptive_refinement_interval= prm.get_integer ("Time steps between mesh refinement");

    generate_graphical_output   = prm.get_bool ("Generate graphical output");
    graphical_output_interval   = prm.get_integer ("Time steps between graphical output");

    prm.enter_subsection ("Discretization");
    {
      navier_stokes_velocity_degree = prm.get_integer ("Navier Stokes velocity polynomial degree");
      use_locally_conservative_discretization
        = prm.get_bool ("Use locally conservative discretization");
    }
    prm.leave_subsection ();
  }

  template <int dim>
  NavierStokes<dim>::NavierStokes (Parameters &parameters_, const RefinementMode refinement_mode)
    :
    parameters (parameters_),
    pcout (std::cout,
           (Utilities::MPI::this_mpi_process(MPI_COMM_WORLD)
            == 0)),

    mapping (4),

    time_step (0),
    old_time_step (0),
    timestep_number (0),
    rebuild_navier_stokes_matrix (true),
    rebuild_navier_stokes_preconditioner (true),

    computing_timer (MPI_COMM_WORLD,
                     pcout,
                     TimerOutput::summary,
                     TimerOutput::wall_times),

    refinement_mode (refinement_mode),

    pgg("Cube"),

    fe_builder("FE_Q"),

    boundary_conditions("Dirichlet boundary conditions"),
    
    right_hand_side("Right-hand side force")
  {}


/*
  template <int dim>
  NavierStokes<dim>::~NavierStokes ()
  {
    // navier_stokes_dof_handler->clear ();
    smart_delete(navier_stokes_dof_handler);
    smart_delete(navier_stokes_fe);
    smart_delete(triangulation);
  }*/


  template <int dim>
  void NavierStokes<dim>::
  setup_navier_stokes_matrix (const std::vector<IndexSet> &navier_stokes_partitioning,
                       const std::vector<IndexSet> &navier_stokes_relevant_partitioning)
  {
    navier_stokes_matrix.clear ();

    TrilinosWrappers::BlockSparsityPattern sp(navier_stokes_partitioning, navier_stokes_partitioning,
                                              navier_stokes_relevant_partitioning,
                                              MPI_COMM_WORLD);

    Table<2,DoFTools::Coupling> coupling (dim+1, dim+1);
    for (unsigned int c=0; c<dim+1; ++c)
      for (unsigned int d=0; d<dim+1; ++d)
        if (! ((c==dim) && (d==dim)))
          coupling[c][d] = DoFTools::always;
        else
          coupling[c][d] = DoFTools::none;

    DoFTools::make_sparsity_pattern (*navier_stokes_dof_handler,
                                     coupling, sp,
                                     navier_stokes_constraints, false,
                                     Utilities::MPI::
                                     this_mpi_process(MPI_COMM_WORLD));
    sp.compress();

    navier_stokes_matrix.reinit (sp);
  }


  template <int dim>
  void NavierStokes<dim>::
  setup_navier_stokes_preconditioner (const std::vector<IndexSet> &navier_stokes_partitioning,
                               const std::vector<IndexSet> &navier_stokes_relevant_partitioning)
  {
    Amg_preconditioner.reset ();
    Mp_preconditioner.reset ();

    navier_stokes_preconditioner_matrix.clear ();

    TrilinosWrappers::BlockSparsityPattern sp(navier_stokes_partitioning, navier_stokes_partitioning,
                                              navier_stokes_relevant_partitioning,
                                              MPI_COMM_WORLD);

    Table<2,DoFTools::Coupling> coupling (dim+1, dim+1);
    for (unsigned int c=0; c<dim+1; ++c)
      for (unsigned int d=0; d<dim+1; ++d)
        if (c == d)
          coupling[c][d] = DoFTools::always;
        else
          coupling[c][d] = DoFTools::none;

    DoFTools::make_sparsity_pattern (*navier_stokes_dof_handler,
                                     coupling, sp,
                                     navier_stokes_constraints, false,
                                     Utilities::MPI::
                                     this_mpi_process(MPI_COMM_WORLD));
    sp.compress();

    navier_stokes_preconditioner_matrix.reinit (sp);
  }


  template <int dim>
  void NavierStokes<dim>::setup_dofs ()
  {
    computing_timer.enter_section("Setup dof systems");

    std::vector<unsigned int> navier_stokes_sub_blocks (dim+1,0);
    navier_stokes_sub_blocks[dim] = 1;
    navier_stokes_dof_handler->distribute_dofs (*navier_stokes_fe);
    DoFRenumbering::component_wise (*navier_stokes_dof_handler, navier_stokes_sub_blocks);

    std::vector<types::global_dof_index> navier_stokes_dofs_per_block (2);
    DoFTools::count_dofs_per_block (*navier_stokes_dof_handler, navier_stokes_dofs_per_block,
                                    navier_stokes_sub_blocks);

    const unsigned int n_u = navier_stokes_dofs_per_block[0],
                       n_p = navier_stokes_dofs_per_block[1];

    std::locale s = pcout.get_stream().getloc();
    pcout.get_stream().imbue(std::locale(""));
    pcout << "Number of active cells: "
          << triangulation->n_global_active_cells()
          << " (on "
          << triangulation->n_levels()
          << " levels)"
          << std::endl
          << "Number of degrees of freedom: "
          << n_u + n_p
          << " (" << n_u << '+' << n_p
          << ')'
          << std::endl
          << std::endl;
    pcout.get_stream().imbue(s);


    std::vector<IndexSet> navier_stokes_partitioning, navier_stokes_relevant_partitioning;
    IndexSet navier_stokes_relevant_set;
    {
      IndexSet navier_stokes_index_set = navier_stokes_dof_handler->locally_owned_dofs();
      navier_stokes_partitioning.push_back(navier_stokes_index_set.get_view(0,n_u));
      navier_stokes_partitioning.push_back(navier_stokes_index_set.get_view(n_u,n_u+n_p));

      DoFTools::extract_locally_relevant_dofs (*navier_stokes_dof_handler,
                                               navier_stokes_relevant_set);
      navier_stokes_relevant_partitioning.push_back(navier_stokes_relevant_set.get_view(0,n_u));
      navier_stokes_relevant_partitioning.push_back(navier_stokes_relevant_set.get_view(n_u,n_u+n_p));

    }

    {
      navier_stokes_constraints.clear ();
      navier_stokes_constraints.reinit (navier_stokes_relevant_set);

      DoFTools::make_hanging_node_constraints (*navier_stokes_dof_handler,
                                               navier_stokes_constraints);

      FEValuesExtractors::Vector velocity_components(0);
      //boundary_conditions.set_time(time_step*time_step_number);
      VectorTools::interpolate_boundary_values (*navier_stokes_dof_handler,
                                                0,
                                                boundary_conditions,
                                                navier_stokes_constraints,
                                                navier_stokes_fe->component_mask(velocity_components));

      navier_stokes_constraints.close ();
    }

    setup_navier_stokes_matrix (navier_stokes_partitioning, navier_stokes_relevant_partitioning);
    setup_navier_stokes_preconditioner (navier_stokes_partitioning,
                                 navier_stokes_relevant_partitioning);

    navier_stokes_rhs.reinit (navier_stokes_partitioning, navier_stokes_relevant_partitioning,
                       MPI_COMM_WORLD, true);
    navier_stokes_solution.reinit (navier_stokes_relevant_partitioning, MPI_COMM_WORLD);
    old_navier_stokes_solution.reinit (navier_stokes_solution);

    rebuild_navier_stokes_matrix              = true;
    rebuild_navier_stokes_preconditioner      = true;

    computing_timer.exit_section();
  }



  template <int dim>
  void
  NavierStokes<dim>::
  local_assemble_navier_stokes_preconditioner (const typename DoFHandler<dim>::active_cell_iterator &cell,
                                        Assembly::Scratch::NavierStokesPreconditioner<dim> &scratch,
                                        Assembly::CopyData::NavierStokesPreconditioner<dim> &data)
  {
    const unsigned int   dofs_per_cell   = navier_stokes_fe->dofs_per_cell;
    const unsigned int   n_q_points      = scratch.navier_stokes_fe_values.n_quadrature_points;

    const FEValuesExtractors::Vector velocities (0);
    const FEValuesExtractors::Scalar pressure (dim);

    scratch.navier_stokes_fe_values.reinit (cell);
    cell->get_dof_indices (data.local_dof_indices);

    data.local_matrix = 0;

    for (unsigned int q=0; q<n_q_points; ++q)
      {
        for (unsigned int k=0; k<dofs_per_cell; ++k)
          {
            scratch.grad_phi_u[k] = scratch.navier_stokes_fe_values[velocities].gradient(k,q);
            scratch.phi_p[k]      = scratch.navier_stokes_fe_values[pressure].value (k, q);
          }

        for (unsigned int i=0; i<dofs_per_cell; ++i)
          for (unsigned int j=0; j<dofs_per_cell; ++j)
            data.local_matrix(i,j) += (EquationData::eta *
                                       scalar_product (scratch.grad_phi_u[i],
                                                       scratch.grad_phi_u[j])
                                       +
                                       (1./EquationData::eta) *
                                       (scratch.phi_p[i] * scratch.phi_p[j]))
                                      * scratch.navier_stokes_fe_values.JxW(q);
      }
  }

  template <int dim>
  void
  NavierStokes<dim>::
  copy_local_to_global_navier_stokes_preconditioner (const Assembly::CopyData::NavierStokesPreconditioner<dim> &data)
  {
    navier_stokes_constraints.distribute_local_to_global (data.local_matrix,
                                                   data.local_dof_indices,
                                                   navier_stokes_preconditioner_matrix);
  }

  template <int dim>
  void
  NavierStokes<dim>::assemble_navier_stokes_preconditioner ()
  {
    navier_stokes_preconditioner_matrix = 0;

    const QGauss<dim> quadrature_formula(parameters.navier_stokes_velocity_degree+1);

    typedef
    FilteredIterator<typename DoFHandler<dim>::active_cell_iterator>
    CellFilter;

    WorkStream::
    run (CellFilter (IteratorFilters::LocallyOwnedCell(),
                     navier_stokes_dof_handler->begin_active()),
         CellFilter (IteratorFilters::LocallyOwnedCell(),
                     navier_stokes_dof_handler->end()),
         std_cxx11::bind (&NavierStokes<dim>::
                          local_assemble_navier_stokes_preconditioner,
                          this,
                          std_cxx11::_1,
                          std_cxx11::_2,
                          std_cxx11::_3),
         std_cxx11::bind (&NavierStokes<dim>::
                          copy_local_to_global_navier_stokes_preconditioner,
                          this,
                          std_cxx11::_1),
         Assembly::Scratch::
         NavierStokesPreconditioner<dim> (*navier_stokes_fe, quadrature_formula,
                                    mapping,
                                    update_JxW_values |
                                    update_values |
                                    update_gradients),
         Assembly::CopyData::
         NavierStokesPreconditioner<dim> (*navier_stokes_fe));

    navier_stokes_preconditioner_matrix.compress(VectorOperation::add);
  }


  template <int dim>
  void
  NavierStokes<dim>::build_navier_stokes_preconditioner ()
  {
    if (rebuild_navier_stokes_preconditioner == false)
      return;

    computing_timer.enter_section ("   Build Stokes preconditioner");
    pcout << "   Rebuilding Stokes preconditioner..." << std::flush;

    assemble_navier_stokes_preconditioner ();

    std::vector<std::vector<bool> > constant_modes;
    FEValuesExtractors::Vector velocity_components(0);
    DoFTools::extract_constant_modes (*navier_stokes_dof_handler,
                                      navier_stokes_fe->component_mask(velocity_components),
                                      constant_modes);

    Mp_preconditioner.reset  (new TrilinosWrappers::PreconditionJacobi());
    Amg_preconditioner.reset (new TrilinosWrappers::PreconditionAMG());

    TrilinosWrappers::PreconditionAMG::AdditionalData Amg_data;
    Amg_data.constant_modes = constant_modes;
    Amg_data.elliptic = true;
    Amg_data.higher_order_elements = true;
    Amg_data.smoother_sweeps = 2;
    Amg_data.aggregation_threshold = 0.02;

    Mp_preconditioner->initialize (navier_stokes_preconditioner_matrix.block(1,1));
    Amg_preconditioner->initialize (navier_stokes_preconditioner_matrix.block(0,0),
                                    Amg_data);

    rebuild_navier_stokes_preconditioner = false;

    pcout << std::endl;
    computing_timer.exit_section();
  }


  template <int dim>
  void
  NavierStokes<dim>::
  local_assemble_navier_stokes_system (const typename DoFHandler<dim>::active_cell_iterator &cell,
                                Assembly::Scratch::NavierStokesSystem<dim> &scratch,
                                Assembly::CopyData::NavierStokesSystem<dim> &data)
  {
    const unsigned int dofs_per_cell = scratch.navier_stokes_fe_values.get_fe().dofs_per_cell;
    const unsigned int n_q_points    = scratch.navier_stokes_fe_values.n_quadrature_points;

    const FEValuesExtractors::Vector velocities (0);
    const FEValuesExtractors::Scalar pressure (dim);

    scratch.navier_stokes_fe_values.reinit (cell);

    std::vector<Vector<double> > rhs_values (n_q_points,
                                             Vector<double>(dim+1));

    right_hand_side.vector_value_list (scratch.navier_stokes_fe_values
				       .get_quadrature_points(),
                                       rhs_values);

    if (rebuild_navier_stokes_matrix)
      data.local_matrix = 0;
      data.local_rhs = 0;


    for (unsigned int q=0; q<n_q_points; ++q)
      {
        for (unsigned int k=0; k<dofs_per_cell; ++k)
          {
            scratch.phi_u[k] = scratch.navier_stokes_fe_values[velocities].value (k,q);
            if (rebuild_navier_stokes_matrix)
              {
                scratch.grads_phi_u[k] = scratch.navier_stokes_fe_values[velocities].symmetric_gradient(k,q);
                scratch.div_phi_u[k]   = scratch.navier_stokes_fe_values[velocities].divergence (k, q);
                scratch.phi_p[k]       = scratch.navier_stokes_fe_values[pressure].value (k, q);
              } 
          }

        if (rebuild_navier_stokes_matrix == true)
          for (unsigned int i=0; i<dofs_per_cell; ++i) {
            for (unsigned int j=0; j<dofs_per_cell; ++j)
              data.local_matrix(i,j) += (EquationData::eta * 2 *
                                        (scratch.grads_phi_u[i] * scratch.grads_phi_u[j])
                                        - (scratch.div_phi_u[i] * scratch.phi_p[j])
                                        - (scratch.phi_p[i] * scratch.div_phi_u[j]))
                                        * scratch.navier_stokes_fe_values.JxW(q);
	
	    unsigned int comp_i = navier_stokes_fe->system_to_component_index(i).first;
	    if(comp_i<dim)
	      data.local_rhs(i) += (rhs_values[q](comp_i) *
				    scratch.phi_u[i][comp_i] *
				    scratch.navier_stokes_fe_values.JxW(q));
	  }
      }

    cell->get_dof_indices (data.local_dof_indices);
  }



  template <int dim>
  void
  NavierStokes<dim>::
  copy_local_to_global_navier_stokes_system (const Assembly::CopyData::NavierStokesSystem<dim> &data)
  {
    if (rebuild_navier_stokes_matrix == true)
      navier_stokes_constraints.distribute_local_to_global (data.local_matrix,
                                                     data.local_rhs,
                                                     data.local_dof_indices,
                                                     navier_stokes_matrix,
                                                     navier_stokes_rhs);
    else
      navier_stokes_constraints.distribute_local_to_global (data.local_rhs,
                                                     data.local_dof_indices,
                                                     navier_stokes_rhs);
  }



  template <int dim>
  void NavierStokes<dim>::assemble_navier_stokes_system ()
  {
    computing_timer.enter_section ("   Assemble Stokes system");

    if (rebuild_navier_stokes_matrix == true)
      navier_stokes_matrix=0;

    navier_stokes_rhs=0;

    const QGauss<dim> quadrature_formula(parameters.navier_stokes_velocity_degree+1);

    typedef
    FilteredIterator<typename DoFHandler<dim>::active_cell_iterator>
    CellFilter;

    WorkStream::
    run (CellFilter (IteratorFilters::LocallyOwnedCell(),
                     navier_stokes_dof_handler->begin_active()),
         CellFilter (IteratorFilters::LocallyOwnedCell(),
                     navier_stokes_dof_handler->end()),
         std_cxx11::bind (&NavierStokes<dim>::
                          local_assemble_navier_stokes_system,
                          this,
                          std_cxx11::_1,
                          std_cxx11::_2,
                          std_cxx11::_3),
         std_cxx11::bind (&NavierStokes<dim>::
                          copy_local_to_global_navier_stokes_system,
                          this,
                          std_cxx11::_1),
         Assembly::Scratch::
         NavierStokesSystem<dim> (*navier_stokes_fe, mapping, quadrature_formula,
                            (update_values    |
                             update_quadrature_points  |
                             update_JxW_values |
                             (rebuild_navier_stokes_matrix == true
                              ?
                              update_gradients
                              :
                              UpdateFlags(0)))),
         Assembly::CopyData::
         NavierStokesSystem<dim> (*navier_stokes_fe));

    if (rebuild_navier_stokes_matrix == true)
      navier_stokes_matrix.compress(VectorOperation::add);
    navier_stokes_rhs.compress(VectorOperation::add);

    rebuild_navier_stokes_matrix = false;

    pcout << std::endl;
    computing_timer.exit_section();
  }

  template <int dim>
  void NavierStokes<dim>::solve ()
  {

      pcout << "   Solving Stokes system... " << std::flush;

      TrilinosWrappers::MPI::BlockVector
      distributed_navier_stokes_solution (navier_stokes_rhs);
      distributed_navier_stokes_solution = navier_stokes_solution;

      const unsigned int
      start = (distributed_navier_stokes_solution.block(0).size() +
               distributed_navier_stokes_solution.block(1).local_range().first),
      end   = (distributed_navier_stokes_solution.block(0).size() +
               distributed_navier_stokes_solution.block(1).local_range().second);
      for (unsigned int i=start; i<end; ++i)
        if (navier_stokes_constraints.is_constrained (i))
          distributed_navier_stokes_solution(i) = 0;


      PrimitiveVectorMemory<TrilinosWrappers::MPI::BlockVector> mem;

      unsigned int n_iterations = 0;
      const double solver_tolerance = 1e-8 * navier_stokes_rhs.l2_norm();
      //SolverControl solver_control (30, solver_tolerance);
      SolverControl solver_control (100, solver_tolerance);

      try
        {
          const LinearSolvers::BlockSchurPreconditioner<TrilinosWrappers::PreconditionAMG,
                TrilinosWrappers::PreconditionJacobi>
                preconditioner (navier_stokes_matrix, navier_stokes_preconditioner_matrix,
                                *Mp_preconditioner, *Amg_preconditioner,
                                false);

          SolverFGMRES<TrilinosWrappers::MPI::BlockVector>
          solver(solver_control, mem,
                 SolverFGMRES<TrilinosWrappers::MPI::BlockVector>::
                 AdditionalData(30, true));

          solver.solve(navier_stokes_matrix, distributed_navier_stokes_solution, navier_stokes_rhs,
                       preconditioner);

          n_iterations = solver_control.last_step();
        }

      catch (SolverControl::NoConvergence)
        {
          const LinearSolvers::BlockSchurPreconditioner<TrilinosWrappers::PreconditionAMG,
                TrilinosWrappers::PreconditionJacobi>
                preconditioner (navier_stokes_matrix, navier_stokes_preconditioner_matrix,
                                *Mp_preconditioner, *Amg_preconditioner,
                                true);

          SolverControl solver_control_refined (navier_stokes_matrix.m(), solver_tolerance);

          SolverFGMRES<TrilinosWrappers::MPI::BlockVector>
          solver(solver_control_refined, mem,
                 SolverFGMRES<TrilinosWrappers::MPI::BlockVector>::
                 AdditionalData(50, true));


          solver.solve(navier_stokes_matrix, distributed_navier_stokes_solution, navier_stokes_rhs,
                       preconditioner);

          n_iterations = (solver_control.last_step() +
                          solver_control_refined.last_step());
        }


      navier_stokes_constraints.distribute (distributed_navier_stokes_solution);

      navier_stokes_solution = distributed_navier_stokes_solution;
      pcout << n_iterations  << " iterations."
            << std::endl;
  }



  template <int dim>
  class NavierStokes<dim>::Postprocessor : public DataPostprocessor<dim>
  {
  public:
    Postprocessor (const unsigned int partition,
                   const double       minimal_pressure);

    virtual
    void
    compute_derived_quantities_vector (const std::vector<Vector<double> >              &uh,
                                       std::vector<Vector<double> >                    &computed_quantities) const;

    virtual std::vector<std::string> get_names () const;

    virtual
    std::vector<DataComponentInterpretation::DataComponentInterpretation>
    get_data_component_interpretation () const;

    virtual UpdateFlags get_needed_update_flags () const;

  private:
    const unsigned int partition;
    const double       minimal_pressure;
  };


  template <int dim>
  NavierStokes<dim>::Postprocessor::
  Postprocessor (const unsigned int partition,
                 const double       minimal_pressure)
    :
    partition (partition),
    minimal_pressure (minimal_pressure)
  {}


  template <int dim>
  std::vector<std::string>
  NavierStokes<dim>::Postprocessor::get_names() const
  {
    std::vector<std::string> solution_names (dim, "velocity");
      solution_names.push_back ("p");
      solution_names.push_back ("partition");
      
    return solution_names;
  }


  template <int dim>
  std::vector<DataComponentInterpretation::DataComponentInterpretation>
  NavierStokes<dim>::Postprocessor::
  get_data_component_interpretation () const
  {
    std::vector<DataComponentInterpretation::DataComponentInterpretation>
    interpretation (dim,
                    DataComponentInterpretation::component_is_part_of_vector);
    interpretation.push_back (DataComponentInterpretation::component_is_scalar);
    interpretation.push_back (DataComponentInterpretation::component_is_scalar);

    return interpretation;
  }


  template <int dim>
  UpdateFlags
  NavierStokes<dim>::Postprocessor::get_needed_update_flags() const
  {
    return update_values | update_gradients | update_q_points;
  }


  template <int dim>
  void
  NavierStokes<dim>::Postprocessor::
  compute_derived_quantities_vector (const std::vector<Vector<double> >              &uh,
                                     std::vector<Vector<double> >                    &computed_quantities) const
  {
    const unsigned int n_quadrature_points = uh.size();
    Assert (computed_quantities.size() == n_quadrature_points,  ExcInternalError());
    Assert (uh[0].size() == dim+1,                              ExcInternalError());

    for (unsigned int q=0; q<n_quadrature_points; ++q)
      {
        for (unsigned int d=0; d<dim; ++d)
          computed_quantities[q](d)
            = (uh[q](d));

        const double pressure = (uh[q](dim)-minimal_pressure);
        computed_quantities[q](dim) = pressure;
        computed_quantities[q](dim+1) = partition;
      }
  }


  template <int dim>
  void NavierStokes<dim>::output_results ()
  {
    computing_timer.enter_section ("Postprocessing");

    Postprocessor postprocessor (Utilities::MPI::this_mpi_process(MPI_COMM_WORLD),
                                 navier_stokes_solution.block(1).minimal_value());

    DataOut<dim> data_out;
data_out.attach_dof_handler (*navier_stokes_dof_handler);
    data_out.add_data_vector (navier_stokes_solution, postprocessor);
    // data_out.build_patches ();

    static int out_index=0;
    const std::string filename = ("solution-" +
                                  Utilities::int_to_string (out_index, 5) +
                                  "." +
                                  Utilities::int_to_string
                                  (triangulation->locally_owned_subdomain(), 4) +
                                  ".vtu");
    std::ofstream output (filename.c_str());
    data_out.write_vtu (output);


    if (Utilities::MPI::this_mpi_process(MPI_COMM_WORLD) == 0)
      {
        std::vector<std::string> filenames;
        for (unsigned int i=0; i<Utilities::MPI::n_mpi_processes(MPI_COMM_WORLD); ++i)
          filenames.push_back (std::string("solution-") +
                               Utilities::int_to_string (out_index, 5) +
                               "." +
                               Utilities::int_to_string(i, 4) +
                               ".vtu");
        const std::string
        pvtu_master_filename = ("solution-" +
                                Utilities::int_to_string (out_index, 5) +
                                ".pvtu");
        std::ofstream pvtu_master (pvtu_master_filename.c_str());
        data_out.write_pvtu_record (pvtu_master, filenames);

        const std::string
        visit_master_filename = ("solution-" +
                                 Utilities::int_to_string (out_index, 5) +
                                 ".visit");
        std::ofstream visit_master (visit_master_filename.c_str());
        data_out.write_visit_record (visit_master, filenames);
      }

    computing_timer.exit_section ();
    out_index++;
  }

  template <int dim>
  void NavierStokes<dim>::refine_mesh ()
  {
    switch (refinement_mode)
      {
      case global_refinement:
      {
        triangulation->refine_global (1);
        // triangulation->refine_global (2);
        break;
      }

      case adaptive_refinement:
      {
        triangulation->refine_global (1);
        // triangulation->refine_global (2);
        break;
      }

      default:
      {
        Assert (false, ExcNotImplemented());
      }
      }
  }

  template <int dim>
  void NavierStokes<dim>::make_grid_fe()
  {

    triangulation = SP(pgg.distributed(MPI_COMM_WORLD));
    navier_stokes_dof_handler = SP(new DoFHandler<dim>(*triangulation));

    //triangulation = pgg.distributed(MPI_COMM_WORLD);

    global_Omega_diameter = GridTools::diameter (*triangulation);

    // navier_stokes_dof_handler = new DoFHandler<dim>(*triangulation);

    navier_stokes_fe=SP(fe_builder());

    triangulation->refine_global (parameters.initial_global_refinement);
    
  }
  
  template <int dim>
  void NavierStokes<dim>::process_solution ()
  {
    eh.error_from_exact(*navier_stokes_dof_handler, navier_stokes_solution, Solution<dim>(), refinement_mode);
  }

template <int dim>
void NavierStokes<dim>::run ()
{

   const unsigned int n_cycles = (refinement_mode==global_refinement)?5:9;
    for (unsigned int cycle=0; cycle<n_cycles; ++cycle)
    {
      if (cycle == 0)
        {
          make_grid_fe ();
        }
      else
        refine_mesh ();

      setup_dofs ();
      assemble_navier_stokes_system ();
      build_navier_stokes_preconditioner ();
      solve ();
      process_solution ();
      output_results ();
    }
    
    // std::ofstream f("errors.txt");
    eh.output_table(std::cout, refinement_mode);
    // f.close();
}


// template class NavierStokes<1>;
template class NavierStokes<2>;
// template class NavierStokes<3>;