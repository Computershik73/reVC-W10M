/* What w10m/bench/d3dbench.cpp needs of a host, for the Windows Phone 8.1 trial (trial.cpp
 * has it). The game's own host_api.h is the shell's, with much more in it. */
#ifndef REVC_WP81_HOST_API_H
#define REVC_WP81_HOST_API_H

#ifdef __cplusplus
extern "C" {
#endif

/* UTF-8, one line: to LocalState\trial.log and to the screen */
void host_log(const char* text);
void host_logf(const char* format, ...);

/* w10m/bench/d3dbench.cpp: REVC_BENCH unset, the draw costs and what the device has;
 * REVC_BENCH=fill, what a layer of pixels and a grid of triangles cost */
void W10M_Direct3DBenchmark(void);

#ifdef __cplusplus
}
#endif

#endif
