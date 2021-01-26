#ifndef __PERFCTR_HALIDE_H__
#define __PERFCTR_HALIDE_H__

# ifndef PERFCTR_VERBOSE
#   define PERFCTR_PRINT
# else
#   define PERFCTR_PRINT           printf
# endif

#define _perfctr_stringify(x) #x
#define _perfctr_expand_and_stringify(x) _perf_stringify(x)
#define perfctr_assert(cond)                                                                               \
  if(!(cond)) {                                                                                         \
    fprintf(stderr, __FILE__ ":" _perfctr_expand_and_stringify(__LINE__) " Assert failed: " #cond "\n");   \
    exit(-1);                                                                                           \
  }

#define MAX_PERFCTR_EVENTS         128
#define MAX_PERFCTR_DESCRIPTORS    128
#define MAX_PERFCTR_THREADS        32

extern "C" {

/* Initialization and markers */
extern int perfctr_halide_initialize();
extern int perfctr_halide_marker_register(const char *, int, int);
extern int perfctr_halide_marker_start(const char *, int, int);
extern int perfctr_halide_marker_stop(const char *, int, int);
extern void perfctr_halide_shutdown();

}

#endif
