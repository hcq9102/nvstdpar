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
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
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

#include "commons.hpp"
#include <experimental/mdspan>
#include "argparse/argparse.hpp"
#include <stdexec/execution.hpp>
#include "exec/static_thread_pool.hpp"

using namespace std;
using namespace stdexec;
using stdexec::sync_wait;

// data type
using Real_t = double;

// number of dimensions
constexpr int dims = 2;

// 2D view
using view_2d = std::extents<int, std::dynamic_extent, std::dynamic_extent>;

// 3D view
using view_3d = std::extents<int, std::dynamic_extent, std::dynamic_extent, std::dynamic_extent>;

// macros to get x and y positions from indices
#define pos(i, ghosts, dx)      -0.5 + dx * (i-ghosts)

// parameters
struct heat_params_t : public argparse::Args
{
    int &ncells = kwarg("n,ncells", "number of cells on each side of the domain").set_default(32);
    int &nsteps = kwarg("s,nsteps", "total steps in simulation").set_default(100);
    Real_t &alpha = kwarg("a,alpha", "thermal diffusivity").set_default(0.5f);
    Real_t &dt = kwarg("t,dt", "time step").set_default(1.0e-5f);
    bool &help = kwarg("h, help", "print help").set_default(false);
    // future use if needed
    // int &max_grid_size = kwarg("g, max_grid_size", "size of each box (or grid)").set_default(32);
    // bool &verbose = kwarg("v, verbose", "verbose mode").set_default(false);
    // int &plot_int = kwarg("p, plot_int", "how often to write a plotfile").set_default(-1);
};

template <typename T> void printGrid(T *grid, int len)
{
    auto view = std::mdspan<T, view_2d, std::layout_right> (grid, len, len);
    std::cout << "Grid: " << std::endl;
    std::cout << std::fixed << std::showpoint;
    std::cout << std::setprecision(2);

    for (auto j = 0; j < view.extent(1); ++j)
    {
        for (auto i = 0; i < view.extent(0); ++i)
        {
            std::cout << view(i, j) << ", ";
        }
        std::cout << std::endl;
    }
    std::cout << std::endl;
}

// fill boundary cells
template <typename T>
void fill2Dboundaries(T *grid, int len, int ghost_cells = 1)
{

    std::for_each_n(std::execution::par_unseq, counting_iterator(ghost_cells), len - ghost_cells, [=](auto i)
    {
        grid[i] = grid[i + (ghost_cells * len)];
        grid[i + (len * (len - ghost_cells))] = grid[i + (len * (len - ghost_cells - 1))];
    });

    std::for_each_n(std::execution::par_unseq, counting_iterator(ghost_cells), len - ghost_cells, [=](auto j)
    {
        grid[j * len] = grid[(ghost_cells * len) + j];
        grid[(len - ghost_cells) + (len * j)] = grid[(len - ghost_cells - 1) + (len * j)];
    });

}

int main(int argc, char *argv[])
{
    // parse params
    heat_params_t args = argparse::parse<heat_params_t>(argc, argv);

    // see if help wanted
    if (args.help)
    {
        args.print(); // prints all variables
        return 0;
    }

    // simulation variables
    int ncells = args.ncells;
    int nsteps = args.nsteps;
    Real_t dt = args.dt;
    Real_t alpha = args.alpha;
    // future if needed to split in multiple grids
    // int max_grid_size = args.max_grid_size;

    // total number of ghost cells = ghosts x dims
    constexpr int ghost_cells = 1;
    constexpr int nghosts = ghost_cells * dims;

    // init simulation time
    Real_t time = 0.0;

    // initialize dx, dy, dz
    auto *dx = new Real_t[dims];
    for (int i = 0; i < dims; ++i)
        dx[i] = 1.0 / (ncells - 1);

    // simulation setup (2D)
    Real_t *grid_old = new Real_t[(ncells+nghosts) * (ncells+nghosts)];
    Real_t *grid_new = new Real_t[(ncells) * (ncells)];

    auto phi_old = std::mdspan<Real_t, view_2d, std::layout_right> (grid_old, ncells + nghosts, ncells + nghosts);
    auto phi_new = std::mdspan<Real_t, view_2d, std::layout_right> (grid_new, ncells, ncells);


    // scheduler from a thread pool
    exec::static_thread_pool ctx{4};

    scheduler auto sch = ctx.get_scheduler();
    sender auto begin = schedule(sch);

    // initialize phi_old domain: {[-0.5, -0.5], [0.5, 0.5]} -> origin at [0,0]
    sender auto heat_eq_sim = then(begin, [&]()
    {
        std::for_each_n(std::execution::par_unseq, counting_iterator(0), ncells*ncells, [=](int pos)
        {
            int i = 1 + (pos / ncells);
            int j = 1 + (pos % ncells);

            Real_t x = pos(i, ghost_cells, dx[0]);
            Real_t y = pos(j, ghost_cells, dx[1]);

            // L2 distance (r2 from origin)
            Real_t r2 = (x*x + y*y)/(0.01);

            // phi(x,y) = 1 + exp(-r^2)
            phi_old(i, j) = 1 + exp(-r2);
        });
    })
    | then([&]()
    {
        // print the initial grid
        printGrid(grid_old, ncells+nghosts);
    })
    | then([&]()
    {
        // evolve the system
        for (auto step = 0; step < nsteps ; step++)
        {
            // fill boundary cells in old_phi
            fill2Dboundaries(grid_old, ncells+nghosts, ghost_cells);

            // update phi_new with stencil
            std::for_each_n(std::execution::par_unseq, counting_iterator(0), ncells*ncells, [=](int pos)
            {
                int i = 1 + (pos / ncells);
                int j = 1 + (pos % ncells);

                // Jacobi iteration
                phi_new(i-1, j-1) = phi_old(i, j) + alpha * dt * (
                        (phi_old(i+1, j) - 2.0 * phi_old(i, j) + phi_old(i-1, j)) / (dx[0] * dx[0]) +
                        (phi_old(i, j+1) - 2.0 * phi_old(i, j) + phi_old(i, j-1)) / (dx[1] * dx[1]));
            });

            // update the simulation time
            time += dt;

            // parallel copy phi_new to phi_old
            std::for_each_n(std::execution::par_unseq, counting_iterator(0), ncells*ncells, [=](int pos)
            {
                int i = 1 + (pos / ncells);
                int j = 1 + (pos % ncells);

                // copy phi_new to phi_old
                phi_old(i, j) = phi_new(i-1, j-1);
            });
        }
    })
    | then([&]()
    {
        // print the final grid
        printGrid(grid_new, ncells);
    })
    | then([&]()
    {
        // delete all memory
        delete[] grid_old;
        delete[] grid_new;

        grid_old = nullptr;
        grid_new = nullptr;
    });

    // start the simulation
    sync_wait(std::move(heat_eq_sim));

    return 0;
}
