#pragma once

#include <dolphin/mtx.h>

#include <cstdint>
#include <memory>

#ifdef __cplusplus
namespace dusk::interp {

void begin_record();
void end_record();
void begin_sim_tick();
uint64_t sim_tick_seq();
void begin_frame(float step);
float get_interpolation_step();

void request_presentation_sync();
bool presentation_sync_active();

bool is_enabled();

bool should_capture();

void record_final_mtx(Mtx m, const void* key);
void record_final_mtx(Mtx m);

void forget_mtx(const void* key);

bool lookup_replacement(const void* key, Mtx out);
bool lookup_concat_replacement(const void* lhs, const void* rhs, Mtx out);

// Replaces a recorded matrix for the current presentation only, ahead of interpolation (used by
// VR to draw what the hands hold at the headset's rate). Cleared when the presentation ends.
void set_presentation_override(const void* key, const Mtx m);

void begin_presentation(float step);
void end_presentation();
bool is_presentation_active();

typedef void (*InterpolationCallBack)(void* pUserWork);
void add_interpolation_callback(InterpolationCallBack pCallBack, void* pUserWork);
void add_interpolation_callback(InterpolationCallBack pCallBack, void* pUserWork, std::shared_ptr<void> owner);

}  // namespace dusk::interp
#endif
