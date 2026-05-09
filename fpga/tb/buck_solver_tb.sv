//=============================================================================
// Testbench: buck_solver_tb
// Purpose:   Verilator C++ harness driver — feeds parameters, runs cycles,
//            compares RTL output against floating-point reference model.
//
// Usage:
//   verilator --cc --exe --build -j $(nproc) -Wall \
//     buck_solver.sv harness_solver.cpp --top-module buck_solver
//   ./obj_dir/Vbuck_solver
//=============================================================================
// NOTE: This is a thin SV wrapper. All stimulus generation and checking
// is done in the C++ harness (harness_solver.cpp).
// Verilator does NOT support @(posedge clk) in tasks/initial — use C++ side.
//=============================================================================

module buck_solver_tb;
    // No simulation logic here — all in C++ harness.
    // This file exists for Questa/VCS/XSim runs if needed later.
    // Verilator only needs the DUT (buck_solver.sv) + harness C++.
endmodule
