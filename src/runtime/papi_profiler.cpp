#include "HalideRuntime.h"
#include "printer.h"
#include "scoped_mutex_lock.h"
#include "papi_profiler.h"

#define PROFILE_GRANULARITY   1

extern "C" {

#ifndef NULL
#define NULL 0
#endif

namespace Halide { namespace Runtime { namespace Internal {

static halide_papi_pipeline_stats *current_pipeline_stats = NULL;

static long long int (*last_counters)[32][128];
static int (*last_counters_used)[32];

WEAK void bill_func(halide_papi_state *s, int func_id, uint64_t time, int active_threads) {
  halide_papi_pipeline_stats *p_prev = NULL;
  for(halide_papi_pipeline_stats *p = s->pipelines; p; p = (halide_papi_pipeline_stats *)(p->next)) {
      if (func_id >= p->first_func_id && func_id < p->first_func_id + p->num_funcs) {
          if (p_prev) {
              // Bubble the pipeline to the top to speed up future queries.
              p_prev->next = (halide_papi_pipeline_stats *)(p->next);
              p->next = s->pipelines;
              s->pipelines = p;
          }
          halide_papi_func_stats *f = p->funcs + func_id - p->first_func_id;
          f->time += time;
          f->active_threads_numerator += active_threads;
          f->active_threads_denominator += 1;
          p->time += time;
          p->samples++;
          p->active_threads_numerator += active_threads;
          p->active_threads_denominator += 1;
          return;
      }
      p_prev = p;
  }
  // Someone must have called reset_state while a kernel was running. Do nothing.
}

WEAK void sampling_profiler_thread(void *) {
  halide_papi_state *s = halide_papi_get_state();

  // grab the lock
  halide_mutex_lock(&s->lock);

  while(s->current_func != halide_papi_please_stop) {
    uint64_t t1 = halide_current_time_ns(NULL);
    uint64_t t = t1;
    while(1) {
      int func, active_threads;
      if (s->get_remote_profiler_state) {
        // Execution has disappeared into remote code running
        // on an accelerator (e.g. Hexagon DSP)
        s->get_remote_profiler_state(&func, &active_threads);
      } else {
        func = s->current_func;
        active_threads = s->active_threads;
      }
      uint64_t t_now = halide_current_time_ns(NULL);
      if (func == halide_papi_please_stop) {
        break;
      } else if (func >= 0) {
        // Assume all time since I was last awake is due to
        // the currently running func.
        bill_func(s, func, t_now - t, active_threads);
      }
      t = t_now;

      // Release the lock, sleep, reacquire.
      int sleep_ms = s->sleep_time;
      halide_mutex_unlock(&s->lock);
      halide_sleep_ms(NULL, sleep_ms);
      halide_mutex_lock(&s->lock);
    }
  }

  halide_mutex_unlock(&s->lock);
}

}}}

// Returns the address of the global halide_papi state
WEAK halide_papi_state *halide_papi_get_state() {
  static halide_papi_state s = {{{0}}, 1, 0, 0, 0, 0, NULL, NULL};
  return &s;
}

WEAK int halide_papi_pipeline_start(
  void *user_context,
  const char *pipeline_name,
  int num_funcs,
  int num_loops,
  const uint64_t *func_names,
  const uint64_t *loop_names,
  const uint64_t *func_show_threads_prod,
  const uint64_t *func_show_threads_cons,
  const uint64_t *loop_show_threads) {

  halide_papi_state *s = halide_papi_get_state();
  halide_papi_pipeline_stats *p;

  for(p = s->pipelines; p; p = (halide_papi_pipeline_stats *)(p->next)) {
    if(p->name == pipeline_name && p->num_funcs == num_funcs) {
      break;
    }
  }

  if(!p) {
    p = (halide_papi_pipeline_stats *) malloc(sizeof(halide_papi_pipeline_stats));
  }

  if(p) {
    p->name = pipeline_name;
    p->next = s->pipelines;
    p->first_func_id = s->first_free_id;
    p->num_funcs = num_funcs;
    p->num_loops = num_loops;
    p->runs = 0;
    p->time = 0;
    p->samples = 0;
    p->active_threads_numerator = 0;
    p->active_threads_denominator = 0;
    p->funcs = (halide_papi_func_stats *) malloc(num_funcs * sizeof(halide_papi_func_stats));
    p->loops = NULL;

    if(p->funcs != NULL) {
      for(int i = 0; i < num_funcs; ++i) {
        p->funcs[i].time = 0;
        p->funcs[i].name = (const char *)(func_names[i]);
        p->funcs[i].active_threads_numerator = 0;
        p->funcs[i].active_threads_denominator = 0;
        p->funcs[i].show_threads_prod = (bool)(func_show_threads_prod[i]);
        p->funcs[i].show_threads_cons = (bool)(func_show_threads_cons[i]);
        p->funcs[i].overhead_iterations = 0;

        for(int l = 0; l < 2; ++l) {
          p->funcs[i].clock_start[l] = 0;
          p->funcs[i].clock_accum[l] = 0;
          p->funcs[i].iterations[l] = 0;

          for(int t = 0; t < MAX_PAPI_THREADS; ++t) {
            p->funcs[i].counter_used[l][t] = 0;
            p->funcs[i].overhead_counter_used[t] = 0;

            for(int e = 0; e < MAX_PAPI_DESCRIPTORS; ++e) {
              p->funcs[i].overhead_counters[t][e] = 0;
              p->funcs[i].event_counters[l][t][e] = 0;
            }
          }
        }
      }
    }

    if(num_loops > 0) {
      p->loops = (halide_papi_loop_stats *) malloc(num_loops * sizeof(halide_papi_loop_stats));

      if(p->loops != NULL) {
        for(int i = 0; i < num_loops; ++i) {
          p->loops[i].name = (const char *)(loop_names[i]);
          p->loops[i].show_threads = (bool)(loop_show_threads[i]);
          p->loops[i].iterations = 0;

          for(int t = 0; t < MAX_PAPI_THREADS; ++t) {
            p->loops[i].loop_counter_used[t] = 0;

            for(int e = 0; e < MAX_PAPI_DESCRIPTORS; ++e) {
              p->loops[i].loop_counters[t][e] = 0;
            }
          }
        }
      }
    }

    if(p->funcs == NULL || (p->loops == NULL && num_loops > 0)) {
      if(p->funcs != NULL) {
        free(p->funcs);
      }

      if(p->loops != NULL) {
        free(p->loops);
      }

      free(p);
      return -1;
    }

    s->first_free_id += num_funcs;
    s->pipelines = p;
  } else {
    return -1;
  }

  ScopedMutexLock lock(&s->lock);

  if (!s->sampling_thread) {
    halide_start_clock(user_context);
    s->sampling_thread = halide_spawn_thread(sampling_profiler_thread, NULL);
  }

  p->runs++;

  current_pipeline_stats = p;
  papi_halide_initialize();
  return p->first_func_id;
}

WEAK void halide_papi_report_func_prod_cons(void *user_context, halide_papi_func_stats *fs, bool is_prod, long long *tot) {
  char line_buf[1024];
  Printer<StringStreamPrinter, sizeof(line_buf)> sstr(user_context, line_buf);
  long long estim_events[MAX_PAPI_EVENTS];
  long long total_events[MAX_PAPI_EVENTS];
  int stage = (is_prod) ? 0 : 1;
  bool show_threads = (is_prod) ? fs->show_threads_prod : fs->show_threads_cons;
  size_t active_threads = 0;

  for(int e = 0; e < papi_halide_number_of_events(); ++e) {
    estim_events[e] = 0;
    total_events[e] = 0;
  }

  for(int th = 0; th < MAX_PAPI_THREADS; ++th) {
    if(fs->counter_used[stage][th] == 1) {
      active_threads++;

      for(int e = 0; e < papi_halide_number_of_events(); ++e) {
        if(fs->iterations[stage] >= PROFILE_GRANULARITY) {
          double ratio = (double)(fs->iterations[stage] % PROFILE_GRANULARITY) / fs->iterations[stage];
          estim_events[e] = (long long)(fs->event_counters[stage][th][e] * (PROFILE_GRANULARITY + ratio));
        } else {
          estim_events[e] = fs->event_counters[stage][th][e] * fs->iterations[stage];
        }

        total_events[e] += estim_events[e];
        tot[e] += estim_events[e];
      }

      if(show_threads) {
        sstr.clear();
        sstr << fs->name;

        if(is_prod) {
          sstr << "_prod";
        } else {
          sstr << "_cons";
        }

        sstr << "_t" << th << ",";

        for(int e = 0; e < papi_halide_number_of_events(); ++e) {
          sstr << estim_events[e] << ",";
        }

        sstr.erase(1);
        sstr << "\n";

        halide_print(user_context, sstr.str());
      }
    }
  }

  if((!show_threads && active_threads == 1) || active_threads > 1) {
    sstr.clear();
    sstr << fs->name;

    if(is_prod) {
      sstr << "_prod_";
    } else {
      sstr << "_cons_";
    }

    if(active_threads > 1) {
      sstr << "total,";
    } else {
      sstr << "t0,";
    }

    for(int e = 0; e < papi_halide_number_of_events(); ++e) {
      sstr << total_events[e] << ",";
    }

    sstr.erase(1);
    sstr << "\n";

    halide_print(user_context, sstr.str());
  }
}

WEAK void halide_papi_report_func_overhead(void *user_context, halide_papi_func_stats *fs, long long *tot) {
  char line_buf[1024];
  Printer<StringStreamPrinter, sizeof(line_buf)> sstr(user_context, line_buf);
  long long estim_events[MAX_PAPI_EVENTS];
  long long total_events[MAX_PAPI_EVENTS];
  size_t active_threads = 0;

  for(int e = 0; e < papi_halide_number_of_events(); ++e) {
    total_events[e] = 0;
  }

  for(int th = 0; th < MAX_PAPI_THREADS; ++th) {
    if(fs->overhead_counter_used[th] == 1) {
      active_threads++;

      for(int e = 0; e < papi_halide_number_of_events(); ++e) {
        if(fs->overhead_iterations >= PROFILE_GRANULARITY) {
          double ratio = (double)(fs->overhead_iterations % PROFILE_GRANULARITY) / fs->overhead_iterations;
          estim_events[e] = (long long)(fs->overhead_counters[th][e] * (PROFILE_GRANULARITY + ratio));
        } else {
          estim_events[e] = fs->overhead_counters[th][e] * fs->overhead_iterations;
        }

        total_events[e] += estim_events[e];
        tot[e] += estim_events[e];
      }

      if(fs->show_threads_prod) {
        sstr.clear();
        sstr << fs->name << "_overhead" << "_t" << th << ",";

        for(int e = 0; e < papi_halide_number_of_events(); ++e) {
          sstr << estim_events[e] << ",";
        }

        sstr.erase(1);
        sstr << "\n";

        halide_print(user_context, sstr.str());
      }
    }
  }

  if((!fs->show_threads_prod && active_threads == 1) || active_threads > 1) {
    sstr.clear();
    sstr << fs->name << "_overhead_";

    if(active_threads > 1) {
      sstr << "total,";
    } else {
      sstr << "t0,";
    }

    for(int e = 0; e < papi_halide_number_of_events(); ++e) {
      sstr << total_events[e] << ",";
    }

    sstr.erase(1);
    sstr << "\n";

    halide_print(user_context, sstr.str());
  }
}

WEAK void halide_papi_report_loop(void *user_context, halide_papi_loop_stats *ls, long long *tot) {
  char line_buf[1024];
  Printer<StringStreamPrinter, sizeof(line_buf)> sstr(user_context, line_buf);
  long long estim_events[MAX_PAPI_EVENTS];
  long long total_events[MAX_PAPI_EVENTS];
  size_t active_threads = 0;

  for(int e = 0; e < papi_halide_number_of_events(); ++e) {
    total_events[e] = 0;
  }

  for(int th = 0; th < MAX_PAPI_THREADS; ++th) {
    if(ls->loop_counter_used[th] == 1) {
      active_threads++;

      for(int e = 0; e < papi_halide_number_of_events(); ++e) {
        if(ls->iterations >= PROFILE_GRANULARITY) {
          double ratio = (double)(ls->iterations % PROFILE_GRANULARITY) / ls->iterations;
          estim_events[e] = (long long)(ls->loop_counters[th][e] * (PROFILE_GRANULARITY + ratio));
        } else {
          estim_events[e] = ls->loop_counters[th][e] * ls->iterations;
        }

        total_events[e] += estim_events[e];
        tot[e] += estim_events[e];
      }

      if(ls->show_threads) {
        sstr.clear();
        sstr << ls->name << "_t" << th << ",";

        for(int e = 0; e < papi_halide_number_of_events(); ++e) {
          sstr << estim_events[e] << ",";
        }

        sstr.erase(1);
        sstr << "\n";

        halide_print(user_context, sstr.str());
      }
    }
  }

  if(!ls->show_threads || active_threads > 1) {
    sstr.clear();
    sstr << ls->name << "_";

    if(active_threads > 1) {
      sstr << "total,";
    } else {
      sstr << "t0,";
    }

    for(int e = 0; e < papi_halide_number_of_events(); ++e) {
      sstr << total_events[e] << ",";
    }

    sstr.erase(1);
    sstr << "\n";

    halide_print(user_context, sstr.str());
  }
}

WEAK void halide_papi_report_unlocked(void *user_context, halide_papi_state *s) {
  char line_buf[1024];
  long long tot[MAX_PAPI_EVENTS];
  Printer<StringStreamPrinter, sizeof(line_buf)> sstr(user_context, line_buf);

  for(halide_papi_pipeline_stats *p = s->pipelines; p; p = (halide_papi_pipeline_stats *)(p->next)) {
    float t = p->time / 1000000.0f;

    if (!p->runs) continue;

    sstr.clear();

    float threads = p->active_threads_numerator / (p->active_threads_denominator + 1e-10);

    sstr << "all," << t << "," << threads << "\n";
    halide_print(user_context, sstr.str());

    for(int i = 1; i < p->num_funcs; i++) {
      halide_papi_func_stats *fs = p->funcs + i;
      float ft = fs->time / (p->runs * 1000000.0f);
      float fthreads = fs->active_threads_numerator / (fs->active_threads_denominator + 1e-10);

      sstr.clear();
      sstr << fs->name << "," << ft << "," << fthreads << "\n";
      halide_print(user_context, sstr.str());

    }

    halide_print(user_context, "\n");

    for(int e = 0; e < papi_halide_number_of_events(); ++e) {
      tot[e] = 0;
    }

    for(int i = 1; i < p->num_funcs; i++) {
      halide_papi_func_stats *fs = p->funcs + i;

      halide_papi_report_func_overhead(user_context, fs, tot);
      halide_papi_report_func_prod_cons(user_context, fs, true, tot);
      halide_papi_report_func_prod_cons(user_context, fs, false, tot);
    }

    for(int i = 0; i < p->num_loops; i++) {
      halide_papi_loop_stats *ls = p->loops + i;
      halide_papi_report_loop(user_context, ls, tot);
    }

    sstr.clear();
    sstr << "total,";

    for(int e = 0; e < papi_halide_number_of_events(); ++e) {
      sstr << tot[e] << ",";
    }

    sstr.erase(1);
    sstr << "\n\n";

    halide_print(user_context, sstr.str());
  }
}

WEAK void halide_papi_report(void *user_context) {
  halide_papi_state *s = halide_papi_get_state();
  ScopedMutexLock lock(&s->lock);
  halide_papi_report_unlocked(user_context, s);
}

WEAK void halide_papi_reset_unlocked(halide_papi_state *s) {
  while (s->pipelines) {
    halide_papi_pipeline_stats *p = s->pipelines;
    s->pipelines = (halide_papi_pipeline_stats *)(p->next);
    free(p->funcs);
    free(p->loops);
    free(p);
  }

  s->first_free_id = 0;
}

WEAK void halide_papi_reset() {
  // WARNING: Do not call this method while any other halide
  // pipeline is running; halide_papi_memory_allocate/free and
  // halide_papi_stack_peak_update update the profiler pipeline's
  // state without grabbing the global profiler state's lock.
  halide_papi_state *s = halide_papi_get_state();
  ScopedMutexLock lock(&s->lock);
  halide_papi_reset_unlocked(s);
}

__attribute__((destructor))  WEAK void halide_papi_shutdown() {
  halide_papi_state *s = halide_papi_get_state();

  if (!s->sampling_thread) {
      return;
  }

  s->current_func = halide_papi_please_stop;
  halide_join_thread(s->sampling_thread);
  s->sampling_thread = NULL;
  s->current_func = halide_papi_outside_of_halide;

  papi_halide_shutdown();

  // Print results. No need to lock anything because we just shut
  // down the thread.
  halide_papi_report_unlocked(NULL, s);

  halide_papi_reset_unlocked(s);
}

WEAK void halide_papi_pipeline_end(void *user_context, void *state) {
  ((halide_profiler_state *)state)->current_func = halide_profiler_outside_of_halide;
  halide_papi_shutdown();
  current_pipeline_stats = NULL;
}

WEAK __attribute__((always_inline)) int halide_papi_set_current_func(halide_papi_state *state, int tok, int t) {
  // Use empty volatile asm blocks to prevent code motion. Otherwise
  // llvm reorders or elides the stores.
  volatile int *ptr = &(state->current_func);
  asm volatile ("":::);
  *ptr = tok + t;
  asm volatile ("":::);
  return 0;
}

WEAK __attribute__((always_inline)) int halide_papi_enter_current_func(halide_papi_state *state, int tok, int t, bool is_producer) {
  halide_papi_func_stats *fs = current_pipeline_stats->funcs + t;
  int level = is_producer ? 0 : 1;

  if((fs->iterations[level] % PROFILE_GRANULARITY) == 0) {
    last_counters = &(fs->event_counters[level]);
    last_counters_used = &(fs->counter_used[level]);
    papi_halide_marker_start();
  }

  return 0;
}

WEAK __attribute__((always_inline)) int halide_papi_leave_current_func(halide_papi_state *state, int tok, int t, bool is_producer) {
  halide_papi_func_stats *fs = current_pipeline_stats->funcs + t;
  int level = is_producer ? 0 : 1;
  int thread_idx = papi_halide_get_thread_index();

  if((fs->iterations[level] % PROFILE_GRANULARITY) == 0) {
    papi_halide_marker_stop(fs->event_counters[level][thread_idx], 1);
    fs->counter_used[level][thread_idx] = 1;
    last_counters = NULL;
  }

  fs->iterations[level]++;
  return 0;
}

WEAK __attribute__((always_inline)) int halide_papi_enter_loop(halide_papi_state *state, int tok, uint64_t id) {
  halide_papi_loop_stats *ls = current_pipeline_stats->loops + id;

  if((ls->iterations % PROFILE_GRANULARITY) == 0) {
    last_counters = &(ls->loop_counters);
    last_counters_used = &(ls->loop_counter_used);
    papi_halide_marker_start();
  }

  return 0;
}

WEAK __attribute__((always_inline)) int halide_papi_leave_loop(halide_papi_state *state, int tok, uint64_t id) {
  halide_papi_loop_stats *ls = current_pipeline_stats->loops + id;
  int thread_idx = papi_halide_get_thread_index();

  if((ls->iterations % PROFILE_GRANULARITY) == 0) {
    papi_halide_marker_stop(ls->loop_counters[thread_idx], 1);
    ls->loop_counter_used[thread_idx] = 1;
    last_counters = NULL;
  }

  ls->iterations++;
  return 0;
}

WEAK __attribute__((always_inline)) int halide_papi_enter_overhead_region(halide_papi_state *state, int tok, int t) {
  halide_papi_func_stats *fs = current_pipeline_stats->funcs + t;

  if((fs->overhead_iterations % PROFILE_GRANULARITY) == 0) {
    last_counters = &(fs->overhead_counters);
    last_counters_used = &(fs->overhead_counter_used);
    papi_halide_marker_start();
  }

  return 0;
}

WEAK __attribute__((always_inline)) int halide_papi_leave_overhead_region(halide_papi_state *state, int tok, int t) {
  halide_papi_func_stats *fs = current_pipeline_stats->funcs + t;
  int thread_idx = papi_halide_get_thread_index();

  if((fs->overhead_iterations % PROFILE_GRANULARITY) == 0) {
    papi_halide_marker_stop(fs->overhead_counters[thread_idx], 1);
    fs->overhead_counter_used[thread_idx] = 1;
    last_counters = NULL;
  }

  fs->overhead_iterations++;
  return 0;
}

WEAK __attribute__((always_inline)) int halide_papi_incr_active_threads(halide_papi_state *state) {
    volatile int *ptr = &(state->active_threads);
    asm volatile ("":::);
    int ret = __sync_fetch_and_add(ptr, 1);
    asm volatile ("":::);

    papi_halide_start_thread();

    return ret;
}

WEAK __attribute__((always_inline)) int halide_papi_decr_active_threads(halide_papi_state *state) {
    int thread_idx = papi_halide_get_thread_index();
    volatile int *ptr = &(state->active_threads);
    asm volatile ("":::);
    int ret = __sync_fetch_and_sub(ptr, 1);
    asm volatile ("":::);

    if(last_counters != NULL) {
      papi_halide_marker_stop((*last_counters)[thread_idx], 1);
      (*last_counters_used)[thread_idx] = 1;
    }

    papi_halide_stop_thread();
    return ret;
}

WEAK __attribute__((always_inline)) int halide_papi_enter_parallel_region(halide_papi_state *state) {
    papi_halide_enter_parallel_region();
    return 0;
}

WEAK __attribute__((always_inline)) int halide_papi_leave_parallel_region(halide_papi_state *state) {
    papi_halide_leave_parallel_region();
    return 0;
}

} // extern "C"
