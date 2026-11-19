#pragma once

/**
 * @file
 * Umbrella header for the physics component vocabulary. It historically
 * carried component declarations; today it forwards to the solver
 * headers that define the domain formulas (currently
 * wengine/solvers/ohm_law.hpp) so existing includes keep resolving.
 * Consumers are any code that historically included this path. Per the
 * dependency rules it must never gain CLI types or parsing knowledge —
 * the direction is cli -> solvers, never the reverse.
 */
// Physics component declarations (resistor, particle, ...).
// Solvers live in wengine/solvers and operate on these + world::State.
#include "wengine/solvers/ohm_law.hpp"
