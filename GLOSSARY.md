# Glossary — WEngine domain language

Living vocabulary for architecture reviews. Terms here name good seams;
new deepened modules take their names from this file.

## Logic module

- **PinRef**: the single pin address (`slot` + `is_output` + `input` + `NQ negated`). Parsed references, mutation targets, tie members, and block boundary entries are all PinRefs (the former `BlockPin` was the same shape under another name).
- **TieSet**: input-input short equivalence classes over dangling input
  pins. Stored as a star in `Netlist::tie_rep` (`0` == singleton, else
  `leader_flat + 1`). Canonical rep is the lowest `(slot, input)`; enumeration
  is slot order. Only seam that may read/write `tie_rep` directly
  (`tie_set.hpp`); everyone else calls `tie/untie/leader/members`.
- **BlockInterface**: derived block boundary, never stored. One block input
  per tie class (canonical leader in slot order) plus dangling outputs in
  slot order, minus `ignore`d pins. Owned by `block_interface.hpp`:
  `block_input/output_count`, `block_interface`, `fill_resolved`,
  `input_index`. Scalar `solve`, bit-parallel `eval_block_word`, the
  sequential stepper, and the CLI poke composer are adapters at this seam.
- **AnchorRow**: role-based sequential input row (1..5 of 6ths of the box
  height, CLK wedge always row 2). Owned once by the `KindPin` table
  (`pins.hpp`); `seq_input_row` and the anchor queries are adapters at
  this seam.
- **SeqSym**: shared sequential symbol model (rect/face/mark/NQ/dots
  behind one interface, `seq_symbol.hpp`). Both raster adapters (SDL +
  ImGui) paint from it; layout anchors stay the router/test seam.
