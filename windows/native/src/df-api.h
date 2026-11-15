#pragma once
#include <stddef.h>

// Hand-transcribed from DeepFilterNet 0.5.6 libDF/src/capi.rs.
// The shipped upstream API requires a valid UTF-8 model file path. NULL is
// undefined behavior; an empty/missing path panics and aborts the process.
// Buffers contain exactly df_get_frame_length() normalized mono floats.
#ifdef __cplusplus
extern "C" {
#endif
typedef struct DFState DFState;
DFState* df_create(const char* path, float atten_lim);
size_t df_get_frame_length(DFState* state);
void df_set_atten_lim(DFState* state, float lim_db);
void df_set_post_filter_beta(DFState* state, float beta);
float df_process_frame(DFState* state, float* input, float* output);
void df_free(DFState* state);
#ifdef __cplusplus
}
#endif
