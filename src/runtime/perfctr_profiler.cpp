#include "HalideRuntime.h"
#include "printer.h"
#include "scoped_mutex_lock.h"
#include "perfctr_profiler.h"

#define PROFILE_GRANULARITY   1

extern "C" {

#ifndef NULL
#define NULL 0
#endif

namespace Halide { namespace Runtime { namespace Internal {

static halide_perfctr_pipeline_stats *current_pipeline_stats = NULL;

/*
WEAK void bill_func(halide_perfctr_state *s, int func_id, uint64_t time, int active_threads) {
  halide_perfctr_pipeline_stats *p_prev = NULL;
  for(halide_perfctr_pipeline_stats *p = s->pipelines; p; p = (halide_perfctr_pipeline_stats *)(p->next)) {
      if (func_id >= p->first_func_id && func_id < p->first_func_id + p->num_funcs) {
          if (p_prev) {
              // Bubble the pipeline to the top to speed up future queries.
              p_prev->next = (halide_perfctr_pipeline_stats *)(p->next);
              p->next = s->pipelines;
              s->pipelines = p;
          }
          halide_perfctr_func_stats *f = p->funcs + func_id - p->first_func_id;
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
  halide_perfctr_state *s = halide_perfctr_get_state();

  // grab the lock
  halide_mutex_lock(&s->lock);

  while(s->current_func != halide_perfctr_please_stop) {
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
      if (func == halide_perfctr_please_stop) {
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
*/
}}}

// Returns the address of the global halide_perfctr state
WEAK halide_perfctr_state *halide_perfctr_get_state() {
    static halide_perfctr_state s = {0, 0, NULL};
    return &s;
}

WEAK int halide_perfctr_pipeline_start(
    void *user_context,
    const char *pipeline_name,
    int num_funcs,
    int num_loops,
    const uint64_t *func_names,
    const uint64_t *loop_names,
    const uint64_t *func_show_threads_prod,
    const uint64_t *func_show_threads_cons,
    const uint64_t *loop_show_threads) {

    halide_perfctr_state *s = halide_perfctr_get_state();
    halide_perfctr_pipeline_stats *p;

    for(p = s->pipelines; p; p = (halide_perfctr_pipeline_stats *)(p->next)) {
        if(p->name == pipeline_name && p->num_funcs == num_funcs) {
            break;
        }
    }

    if(!p) {
        p = (halide_perfctr_pipeline_stats *) malloc(sizeof(halide_perfctr_pipeline_stats));
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
        p->funcs = (halide_perfctr_func_stats *) malloc(num_funcs * sizeof(halide_perfctr_func_stats));
        p->loops = NULL;

        if(p->funcs != NULL) {
            for(int i = 0; i < num_funcs; ++i) {
                p->funcs[i].time = 0;
                p->funcs[i].name = (const char *)(func_names[i]);
                p->funcs[i].overhead_iterations = 0;

                for(int l = 0; l < 2; ++l) {
                    int j;

                    p->funcs[i].clock_start[l] = 0;
                    p->funcs[i].clock_accum[l] = 0;
                    p->funcs[i].iterations[l] = 0;

                    for(j = 0; p->funcs[i].name[j] != '\0'; ++j) {
                        p->funcs[i].marker[l][j] = p->funcs[i].name[j];
                        p->funcs[i].overhead_marker[j] = p->funcs[i].name[j];
                    }

                    p->funcs[i].marker[l][j] = '_';
                    p->funcs[i].overhead_marker[j] = '_';
                    j++;
                    p->funcs[i].marker[l][j] = (l == 0) ? 'p' : 'c';
                    p->funcs[i].overhead_marker[j] = 'o';
                    j++;
                    p->funcs[i].marker[l][j] = (l == 0) ? 'r' : 'o';
                    p->funcs[i].overhead_marker[j] = 'v';
                    j++;
                    p->funcs[i].marker[l][j] = (l == 0) ? 'o' : 'n';
                    p->funcs[i].overhead_marker[j] = 'h';
                    j++;
                    p->funcs[i].marker[l][j] = (l == 0) ? 'd' : 's';
                    p->funcs[i].overhead_marker[j] = 'd';
                    j++;
                    p->funcs[i].marker[l][j] = '\0';
                    p->funcs[i].overhead_marker[j] = '\0';
                }
            }
        }

        if(num_loops > 0) {
            p->loops = (halide_perfctr_loop_stats *) malloc(num_loops * sizeof(halide_perfctr_loop_stats));
            if(p->loops != NULL) {
                for(int i = 0; i < num_loops; ++i) {
                    p->loops[i].name = (const char *)(loop_names[i]);
                    p->loops[i].marker = (const char *)(loop_names[i]);
                    p->loops[i].iterations = 0;
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

    /*
    ScopedMutexLock lock(&s->lock);

    if (!s->sampling_thread) {
    halide_start_clock(user_context);
    s->sampling_thread = halide_spawn_thread(sampling_profiler_thread, NULL);
    }
    */
    p->runs++;

    current_pipeline_stats = p;
    perfctr_halide_initialize();
    return p->first_func_id;
}

/*
WEAK void halide_perfctr_report_func_prod_cons(void *user_context, halide_perfctr_func_stats *fs, bool is_prod, long long *tot) {
  char line_buf[1024];
  Printer<StringStreamPrinter, sizeof(line_buf)> sstr(user_context, line_buf);
  long long estim_events[MAX_PERFCTR_EVENTS];
  long long total_events[MAX_PERFCTR_EVENTS];
  int stage = (is_prod) ? 0 : 1;
  bool show_threads = (is_prod) ? fs->show_threads_prod : fs->show_threads_cons;
  size_t active_threads = 0;

  for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
    estim_events[e] = 0;
    total_events[e] = 0;
  }

  for(int th = 0; th < MAX_PERFCTR_THREADS; ++th) {
    if(fs->counter_used[stage][th] == 1) {
      active_threads++;

      for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
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

        for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
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

    for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
      sstr << total_events[e] << ",";
    }

    sstr.erase(1);
    sstr << "\n";

    halide_print(user_context, sstr.str());
  }
}

WEAK void halide_perfctr_report_func_overhead(void *user_context, halide_perfctr_func_stats *fs, long long *tot) {
  char line_buf[1024];
  Printer<StringStreamPrinter, sizeof(line_buf)> sstr(user_context, line_buf);
  long long estim_events[MAX_PERFCTR_EVENTS];
  long long total_events[MAX_PERFCTR_EVENTS];
  size_t active_threads = 0;

  for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
    total_events[e] = 0;
  }

  for(int th = 0; th < MAX_PERFCTR_THREADS; ++th) {
    if(fs->overhead_counter_used[th] == 1) {
      active_threads++;

      for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
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

        for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
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

    for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
      sstr << total_events[e] << ",";
    }

    sstr.erase(1);
    sstr << "\n";

    halide_print(user_context, sstr.str());
  }
}

WEAK void halide_perfctr_report_loop(void *user_context, halide_perfctr_loop_stats *ls, long long *tot) {
  char line_buf[1024];
  Printer<StringStreamPrinter, sizeof(line_buf)> sstr(user_context, line_buf);
  long long estim_events[MAX_PERFCTR_EVENTS];
  long long total_events[MAX_PERFCTR_EVENTS];
  size_t active_threads = 0;

  for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
    total_events[e] = 0;
  }

  for(int th = 0; th < MAX_PERFCTR_THREADS; ++th) {
    if(ls->loop_counter_used[th] == 1) {
      active_threads++;

      for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
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

        for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
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

    for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
      sstr << total_events[e] << ",";
    }

    sstr.erase(1);
    sstr << "\n";

    halide_print(user_context, sstr.str());
  }
}

WEAK void halide_perfctr_report_unlocked(void *user_context, halide_perfctr_state *s) {
  char line_buf[1024];
  Printer<StringStreamPrinter, sizeof(line_buf)> sstr(user_context, line_buf);

  for(halide_perfctr_pipeline_stats *p = s->pipelines; p; p = (halide_perfctr_pipeline_stats *)(p->next)) {
    float t = p->time / 1000000.0f;

    if (!p->runs) continue;

    sstr.clear();

    float threads = p->active_threads_numerator / (p->active_threads_denominator + 1e-10);

    sstr << "all," << t << "," << threads << "\n";
    halide_print(user_context, sstr.str());

    for(int i = 1; i < p->num_funcs; i++) {
      halide_perfctr_func_stats *fs = p->funcs + i;
      float ft = fs->time / (p->runs * 1000000.0f);
      float fthreads = fs->active_threads_numerator / (fs->active_threads_denominator + 1e-10);

      sstr.clear();
      sstr << fs->name << "," << ft << "," << fthreads << "\n";
      halide_print(user_context, sstr.str());

    }

    halide_print(user_context, "\n");

#ifndef USE_LIKWID
    long long tot[MAX_PERFCTR_EVENTS];

    for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
      tot[e] = 0;
    }

    for(int i = 1; i < p->num_funcs; i++) {
      halide_perfctr_func_stats *fs = p->funcs + i;

      halide_perfctr_report_func_overhead(user_context, fs, tot);
      halide_perfctr_report_func_prod_cons(user_context, fs, true, tot);
      halide_perfctr_report_func_prod_cons(user_context, fs, false, tot);
    }

    for(int i = 0; i < p->num_loops; i++) {
      halide_perfctr_loop_stats *ls = p->loops + i;
      halide_perfctr_report_loop(user_context, ls, tot);
    }

    sstr.clear();
    sstr << "total,";

    for(int e = 0; e < perfctr_halide_number_of_events(); ++e) {
      sstr << tot[e] << ",";
    }

    sstr.erase(1);
    sstr << "\n\n";

    halide_print(user_context, sstr.str());
#endif
  }
}

WEAK void halide_perfctr_report(void *user_context) {
    halide_perfctr_state *s = halide_perfctr_get_state();
    ScopedMutexLock lock(&s->lock);
    halide_perfctr_report_unlocked(user_context, s);
}
*/

WEAK void halide_perfctr_reset_unlocked(halide_perfctr_state *s) {
    while (s->pipelines) {
        halide_perfctr_pipeline_stats *p = s->pipelines;
        s->pipelines = (halide_perfctr_pipeline_stats *)(p->next);
        free(p->funcs);
        free(p->loops);
        free(p);
    }

    s->first_free_id = 0;
}

/*
WEAK void halide_perfctr_reset() {
    // WARNING: Do not call this method while any other halide
    // pipeline is running; halide_perfctr_memory_allocate/free and
    // halide_perfctr_stack_peak_update update the profiler pipeline's
    // state without grabbing the global profiler state's lock.
    halide_perfctr_state *s = halide_perfctr_get_state();
    ScopedMutexLock lock(&s->lock);
    halide_perfctr_reset_unlocked(s);
}
*/

__attribute__((destructor))  WEAK void halide_perfctr_shutdown() {
    halide_perfctr_state *s = halide_perfctr_get_state();

    /*
    if (!s->sampling_thread) {
        return;
    }

    s->current_func = halide_perfctr_please_stop;
    halide_join_thread(s->sampling_thread);
    s->sampling_thread = NULL;
    s->current_func = halide_perfctr_outside_of_halide;

    // Print results. No need to lock anything because we just shut
    // down the thread.
    //halide_perfctr_report_unlocked(NULL, s);
    */

    halide_perfctr_reset_unlocked(s);
    perfctr_halide_shutdown();
}

WEAK void halide_perfctr_pipeline_end(void *user_context, void *state) {
    halide_perfctr_shutdown();
    current_pipeline_stats = NULL;
}

WEAK __attribute__((always_inline)) int halide_perfctr_register_func(halide_perfctr_state *state, int tok, int func, bool is_producer) {
    halide_perfctr_func_stats *fs = current_pipeline_stats->funcs + func;
    int level = is_producer ? 0 : 1;
    perfctr_halide_marker_register(fs->marker[level], func, level);
    return 0;
}

WEAK __attribute__((always_inline)) int halide_perfctr_enter_func(halide_perfctr_state *state, int tok, int func, bool is_producer) {
    halide_perfctr_func_stats *fs = current_pipeline_stats->funcs + func;
    int level = is_producer ? 0 : 1;

    if((fs->iterations[level] % PROFILE_GRANULARITY) == 0) {
        perfctr_halide_marker_start(fs->marker[level], func, level);
    }

    return 0;
}

WEAK __attribute__((always_inline)) int halide_perfctr_leave_func(halide_perfctr_state *state, int tok, int func, bool is_producer) {
    halide_perfctr_func_stats *fs = current_pipeline_stats->funcs + func;
    int level = is_producer ? 0 : 1;
    if((fs->iterations[level] % PROFILE_GRANULARITY) == 0) {
        perfctr_halide_marker_stop(fs->marker[level], func, level);
    }

    fs->iterations[level]++;
    return 0;
}

WEAK __attribute__((always_inline)) int halide_perfctr_register_loop(halide_perfctr_state *state, int tok, int id) {
    halide_perfctr_loop_stats *ls = current_pipeline_stats->loops + id;
    perfctr_halide_marker_register(ls->marker, id, 2);
    return 0;
}

WEAK __attribute__((always_inline)) int halide_perfctr_enter_loop(halide_perfctr_state *state, int tok, int id) {
    halide_perfctr_loop_stats *ls = current_pipeline_stats->loops + id;
    if((ls->iterations % PROFILE_GRANULARITY) == 0) {
        perfctr_halide_marker_start(ls->marker, id, 2);
    }

    return 0;
}

WEAK __attribute__((always_inline)) int halide_perfctr_leave_loop(halide_perfctr_state *state, int tok, int id) {
    halide_perfctr_loop_stats *ls = current_pipeline_stats->loops + id;
    if((ls->iterations % PROFILE_GRANULARITY) == 0) {
        perfctr_halide_marker_stop(ls->marker, id, 2);
    }

    ls->iterations++;
    return 0;
}

WEAK __attribute__((always_inline)) int halide_perfctr_register_overhead_region(halide_perfctr_state *state, int tok, int func) {
    halide_perfctr_func_stats *fs = current_pipeline_stats->funcs + func;
    perfctr_halide_marker_register(fs->overhead_marker, func, 3);
    return 0;
}

WEAK __attribute__((always_inline)) int halide_perfctr_enter_overhead_region(halide_perfctr_state *state, int tok, int func) {
    halide_perfctr_func_stats *fs = current_pipeline_stats->funcs + func;
    if((fs->overhead_iterations % PROFILE_GRANULARITY) == 0) {
        perfctr_halide_marker_start(fs->overhead_marker, func, 3);
    }

    return 0;
}

WEAK __attribute__((always_inline)) int halide_perfctr_leave_overhead_region(halide_perfctr_state *state, int tok, int func) {
    halide_perfctr_func_stats *fs = current_pipeline_stats->funcs + func;
    if((fs->overhead_iterations % PROFILE_GRANULARITY) == 0) {
        perfctr_halide_marker_stop(fs->overhead_marker, func, 3);
    }

    fs->overhead_iterations++;
    return 0;
}

} // extern "C"
