#pragma once

// Remote-debugging support (3.25.0).
//
//  smoothpan diag      — one-shot environment + state report
//                        (dfhack-config/smoothpan/smoothpan_diag.txt)
//  smoothpan selftest  — scripted run over ~20 s in fortress mode: idle
//                        parity (compositor on vs off), smooth zoom in/out,
//                        multi-notch zoom, pan, vanilla zoom + bridge.  Records
//                        per-frame compositor/zoom state and edge black-gap
//                        measurements read back from the final frame, saves
//                        screenshots at checkpoints, writes a verdict report
//                        (smoothpan_selftest.txt).  Drivable through dfhack-run.

#include <SDL.h>
#include <string>

namespace DFHack { class color_ostream; }

void smoothpan_write_diag(std::string& out);

// Start the self-test (returns false + reason when it cannot start).
bool selftest_start(std::string& why);
void selftest_abort(const char* why);
bool selftest_running();
// Once per dwarfmode render, after SmoothCamera::update() and before the zoom
// camera update: applies the scripted inputs for the current step.
void selftest_tick();
// From the SDL_RenderPresent hook, after the compositor finalized the frame
// and before the real present: measurements + screenshots.
void selftest_on_present(SDL_Renderer* r);
// Console line with progress / last verdict.
std::string selftest_status();
// Poll from the render hook to print the completion message on the console.
bool selftest_take_finished(std::string& summary);
