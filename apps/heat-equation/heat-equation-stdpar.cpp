/*
 * MIT License
 *
 * Copyright (c) 2023 The Regents of the University of California,
 * through Lawrence Berkeley National Laboratory (subject to receipt of any
 * required approvals from the U.S. Dept. of Energy).All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/*
 * Simplified 2d heat equation example derived from amrex
 */

#include "heat-equation.hpp"

//
// simulation
//
int main(int argc, char* argv[]) {
  // parse params
  heat_params_t args = argparse::parse<heat_params_t>(argc, argv);

  // see if help wanted
  if (args.help) {
    args.print();  // prints all variables
    return 0;
  }

  // simulation variables
  int ncells = args.ncells;
  int nsteps = args.nsteps;
  Real_t dt = args.dt;
  Real_t alpha = args.alpha;
  // future if needed to split in multiple grids
  // int max_grid_size = args.max_grid_size;

  // initialize dx, dy, dz
  auto* dx = new Real_t[dims];
  for (int i = 0; i < dims; ++i)
    dx[i] = 1.0 / (ncells - 1);

  // simulation setup (2D)
  Real_t* grid_old = new Real_t[(ncells + nghosts) * (ncells + nghosts)];
  Real_t* grid_new = new Real_t[(ncells) * (ncells)];

  auto phi_old = std::mdspan<Real_t, view_2d, std::layout_right>(
      grid_old, ncells + nghosts, ncells + nghosts);
  auto phi_new =
      std::mdspan<Real_t, view_2d, std::layout_right>(grid_new, ncells, ncells);

  // initialize phi_old domain: {[-0.5, -0.5], [0.5, 0.5]} -> origin at [0,0]

  Timer timer;

  std::for_each_n(std::execution::par_unseq, counting_iterator(0),
                  ncells * ncells, [=](int ind) {
                    int i = 1 + (ind / ncells);
                    int j = 1 + (ind % ncells);

                    Real_t x = pos(i, ghost_cells, dx[0]);
                    Real_t y = pos(j, ghost_cells, dx[1]);

                    // L2 distance (r2 from origin)
                    Real_t r2 = (x * x + y * y) / (0.01);

                    // phi(x,y) = 1 + exp(-r^2)
                    phi_old(i, j) = 1 + exp(-r2);
                  });

  if (args.print_grid)
    // print the initial grid
    printGrid(grid_old, ncells + nghosts);

  // init simulation time
  Real_t time = 0.0;

  // evolve the system
  for (auto step = 0; step < nsteps; step++) {
    // fill boundary cells in old_phi
    fill2Dboundaries(grid_old, ncells + nghosts, ghost_cells);

    // update phi_new with stencil
    std::for_each_n(std::execution::par_unseq, counting_iterator(0),
                    ncells * ncells, [=](int ind) {
                      int i = 1 + (ind / ncells);
                      int j = 1 + (ind % ncells);

                      // Jacobi iteration
                      phi_new(i - 1, j - 1) =
                          phi_old(i, j) +
                          alpha * dt *
                              ((phi_old(i + 1, j) - 2.0 * phi_old(i, j) +
                                phi_old(i - 1, j)) /
                                   (dx[0] * dx[0]) +
                               (phi_old(i, j + 1) - 2.0 * phi_old(i, j) +
                                phi_old(i, j - 1)) /
                                   (dx[1] * dx[1]));
                    });

    // update the simulation time
    time += dt;

    // parallel copy phi_new to phi_old
    std::for_each_n(std::execution::par_unseq, counting_iterator(0),
                    ncells * ncells, [=](int ind) {
                      int i = 1 + (ind / ncells);
                      int j = 1 + (ind % ncells);

                      // copy phi_new to phi_old
                      phi_old(i, j) = phi_new(i - 1, j - 1);
                    });
  }

  auto elapsed = timer.stop();

  // print timing
  if (args.print_time) {
    std::cout << "Time: " << elapsed << " ms" << std::endl;
  }

  if (args.print_grid)
    // print the final grid
    printGrid(grid_new, ncells);

  // delete all memory
  delete[] grid_old;
  delete[] grid_new;

  grid_old = nullptr;
  grid_new = nullptr;

  return 0;
}
