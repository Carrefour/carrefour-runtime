/*
Copyright (C) 2013  
Fabien Gaud <fgaud@sfu.ca>, Baptiste Lepers <baptiste.lepers@inria.fr>

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
version 2, as published by the Free Software Foundation.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include "carrefour.h"
#include <gsl/gsl_statistics.h>
#include <sys/sysinfo.h>
#include <sys/time.h>

static const int sleep_time_carrefour_efficient       = 1*TIME_SECOND;     /* Profile by sleep_time useconds chunks */
static const int sleep_time_carrefour_not_efficient   = 5*TIME_SECOND;     /* Profile by sleep_time useconds chunks */
static const int sleep_time_kthp                      = 1*TIME_SECOND;

//static const int sleep_time_kthp       = 200*TIME_MS;

/* Only triggers carrefour if the rate of memory accesses is above the threshold and the IPC is below the other one */
#define MAPTU_MIN                   15

/* Interleaving thresholds */
#define MIN_IMBALANCE               35 /* Deviation in % */
#define MAX_LOCALITY                100 /* In % - We don't want to strongly decrease the locality */

/* Migration threshold */
#define MAX_LOCALITY_MIGRATION      80 /* In % */
/***/

/** For KTHP **/
#define DEFAULT_USE_KTHP            1  // Choose between KTHP and NATHP
#define ALLOC_HUGE_WHEN_ENABLED     1
#define KTHP_WHEN_ENABLED           1
#define TLB_METRIC_THRES            5

#define KEEP_DECISION_FOR_X_TIMES   0 // -1: forever, 0: don't keep, X: number of iterations

#define ENABLE_PGFLT_METRIC         1 // Evaluates the max time spent in the page fault handler on cores
#define PGFLT_METRIC_THRES          5

#define ENABLE_LOCK_CONT_METRIC     0
#define LOCK_CONT_THRESHOLD         5

#define ENABLE_IBS_ASSIST           0

#define EVALUATE_EFFICIENCY         1
#define EVALUATE_EFFICIENCY_STRONG  1
#define EFFICIENCY_THRESHOLD        10 // Worth if it improves by X percent
/***/

/** Internal **/
#define ENABLE_MULTIPLEXING_CHECKS  0
#define MIN_ACTIVE_PERCENTAGE       15
#define VERBOSE                     1
#define MAX_FEEDBACK_LENGTH         256
/**/

#define xstr(a) str(a)
#define str(a) #a

#if !VERBOSE
#define printf(args...) do {} while(0)
#endif

struct decision_t {
   int enable_carrefour;
   int enable_interleaving;
   int enable_replication;
   int enable_migration;

   int enable_splitting;
};

struct decision_t current_decisions;
static int nr_thp_split = 0;

static void sig_handler(int signal);
static long sys_perf_counter_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags);

/*
 * Events :
 * - PERF_TYPE_RAW: raw counters. The value must be 0xz0040yyzz.
 *      For 'z-zz' values, see AMD reference manual (eg. 076h = CPU_CLK_UNHALTED).
 *      'yy' is the Unitmask.
 *      The '4' is 0100b = event is enabled (can also be enable/disabled via ioctl).
 *      The '0' before yy indicate which level to monitor (User or OS).
 *              It is modified by the event_attr when .exclude_[user/kernel] == 0.
 *              When it is the case the bits 16 or 17 of .config are set to 1 (== monitor user/kernel).
 *              If this is set to anything else than '0', it can be confusing since the kernel does not modify it when .exclude_xxx is set.
 *
 * - PERF_TYPE_HARDWARE: predefined values of HW counters in Linux (eg PERF_COUNT_HW_CPU_CYCLES = CPU_CLK_UNHALTED).
 *
 * - leader = -1 : the event is a group leader
 *   leader = x!=-1 : the event is only scheduled when its group leader is scheduled
 */
static event_t per_core_events[] = {
   {   
      .name    = "L2_MISSES_TLB",
      .type    = PERF_TYPE_RAW,
      .config  = 0x47e,
      .leader  = -1,
      .used_by = USED_BY_KTHP, // Should only be used by one or another for now
   },  

   {   
      .name    = "L2_MISSES_ALL",
      .type    = PERF_TYPE_RAW,
      .config  = 0xff7e,
      .leader  = -1,
      .used_by = USED_BY_KTHP, // Should only be used by one or another for now
   },  
   {
      .name    = "minor-faults",
      .type    = PERF_TYPE_SOFTWARE,
      .config  = PERF_COUNT_SW_PAGE_FAULTS_MIN, 
      .leader  = -1,
      .used_by = USED_BY_KTHP, // Should only be used by one or another for now
   }
};

static event_t per_node_events[] = {
   /** LAR & DRAM imbalance **/
#if NB_NODES >= 2
   {
      .name    = "CPU_DRAM_NODE0",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1000001E0,
      .leader  = -1,
      .used_by = USED_BY_CARREFOUR, // Should only be used by one or another for now
   },
   {
      .name    = "CPU_DRAM_NODE1",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1000002E0,
      .leader  = -1,
      .used_by = USED_BY_CARREFOUR, // Should only be used by one or another for now
   },
#endif
#if NB_NODES >= 4
   {
      .name    = "CPU_DRAM_NODE2",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1000004E0,
      .leader  = -1,
      .used_by = USED_BY_CARREFOUR, // Should only be used by one or another for now
   },
   {
      .name    = "CPU_DRAM_NODE3",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1000008E0,
      .leader  = -1,
      .used_by = USED_BY_CARREFOUR, // Should only be used by one or another for now
   },
#endif
#if NB_NODES >= 8
   {
      .name    = "CPU_DRAM_NODE4",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1000010E0,
      .leader  = -1,
      .used_by = USED_BY_CARREFOUR, // Should only be used by one or another for now
   },
   {
      .name    = "CPU_DRAM_NODE5",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1000020E0,
      .leader  = -1,
      .used_by = USED_BY_CARREFOUR, // Should only be used by one or another for now
   },
   {
      .name    = "CPU_DRAM_NODE6",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1000040E0,
      .leader  = -1,
      .used_by = USED_BY_CARREFOUR, // Should only be used by one or another for now
   },
   {
      .name    = "CPU_DRAM_NODE7",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1000080E0,
      .leader  = -1,
      .used_by = USED_BY_CARREFOUR, // Should only be used by one or another for now
   },
#endif
};

static int nb_events_per_core = sizeof(per_core_events)/sizeof(*per_core_events);
static int nb_events_per_node = sizeof(per_node_events)/sizeof(*per_node_events);

static int nb_nodes;
static int nb_cores;

static int enable_carrefour   = 0;
static int enable_kthp        = 0;
static int enable_hybrid      = 0;

static int hugepages_are_enabled = 0;

static uint64_t clk_speed;
static int quiet = 0;

static uint64_t get_cpu_freq(void) {
   FILE *fd;
   uint64_t freq = 0;
   float freqf = 0;
   char *line = NULL;
   size_t len = 0;

   fd = fopen("/proc/cpuinfo", "r");
   if (!fd) {
      printf("failed to get cpu frequency\n");
      perror(NULL);
      return freq;
   }

   while (getline(&line, &len, fd) != EOF) {
      if (sscanf(line, "cpu MHz\t: %f", &freqf) == 1) {
         freqf = freqf * 1000000UL;
         freq = (uint64_t) freqf;
         break;
      }
   }

   fclose(fd);
   return freq;
}

static int cpu_of_node(int node) {
  struct bitmask *bmp;
  int cpu;

  bmp = numa_allocate_cpumask();
  numa_node_to_cpus(node, bmp);
  for(cpu = 0; cpu < nb_cores; cpu++) {
     if (numa_bitmask_isbitset(bmp, cpu)){
        numa_bitmask_free(bmp);
        return cpu;
     }
  }
  numa_bitmask_free(bmp);
  return 0;
}

static inline void change_carrefour_state_str(char * str) {
   if(str) {
      FILE *ibs_ctl = fopen("/proc/inter_cntl", "w");
      if(ibs_ctl) {
         int len = strlen(str);
         fwrite(str, 1, len, ibs_ctl);
         // That's not safe. Todo check for errors
         fclose(ibs_ctl);
      }
      else {
         printf("[WARNING] Cannot open the carrefour file. Is carrefour loaded?\n");
      }
   }
}

static inline void change_carrefour_state(char c) {
   FILE *ibs_ctl = fopen("/proc/inter_cntl", "w");
   if(ibs_ctl) {
      fputc(c, ibs_ctl);
      fclose(ibs_ctl);
   }
   else {
      printf("Cannot open the carrefour file. Is carrefour loaded?\n");
   }
}

static inline int is_carrefour_loaded (void) {
   FILE *ibs_ctl = fopen("/proc/inter_cntl", "w");
   int loaded = 0;
   if(ibs_ctl) {
      loaded = 1;
      fclose(ibs_ctl);
   }

   return loaded;
}

static inline void write_file(char *file, char *string) {
   FILE *ctl = fopen(file, "w");
   if(ctl) {
      fwrite(string, strlen(string)+1, 1, ctl);
      fclose(ctl);
   } else {
      printf("Cannot write %s\n", file);
      exit(EXIT_FAILURE);
   }
}     

static inline void write_file_ulong(char *file, unsigned long value) {
   FILE *ctl = fopen(file, "w");
   if(ctl) {
      fprintf(ctl, "%lu", value);
      fclose(ctl);
   } else {
      printf("Cannot write %s\n", file);
      exit(EXIT_FAILURE);
   }
}     


// Use only unsigned long
struct kernel_feedback_t {
   unsigned long mmlock;
   unsigned long spinlock;
   unsigned long mmap;
   unsigned long brk;
   unsigned long munmap;
   unsigned long mprotect;
   unsigned long pgflt;
   unsigned long migrations;
};

struct module_feedback_t {
   unsigned long split_has_been_chosen;
   unsigned long ran;
   unsigned long nr_thp_split;
   unsigned long thp_state;
};

struct hwc_feedback_t {
   double rr;
   double maptu;
   double lar;
   double imbalance;
   double mem_usage;

   double aggregate_dram_accesses_to_node[NB_NODES];
   double lar_node[NB_NODES];
   double maptu_nodes[NB_NODES];
}; 

struct global_stats_t {
   double aggregate_dram_accesses_to_node[NB_NODES];

   double total_nr_accesses;
   double total_nr_local_accesses;

   double total_nr_L2_misses;
   double total_nr_L2_misses_pgtblewalk;
} global_stats;

static struct hwc_feedback_t previous_hwc_feedback;

static int current_alloc_huge = 0;
static int current_kthp_enabled = 0;

#if KEEP_DECISION_FOR_X_TIMES > 0
static int last_iteration_valid = 0;
#endif

static inline void change_nathp_state(int tlb_metric, struct kernel_feedback_t* feedback, int ibs_decision_feedback) {
   int enabled_because_tlb = tlb_metric >= TLB_METRIC_THRES; 

   int decided_alloc_huge = 0;
   int decided_kthp_enabled = 0;

/*
 * 1st attempt to create a "complete" solution. Works as follows:
 * - 2M pages are enabled by default and never disabled
 * - Carrouf++ splits pages.
 * - If tlb_metric > THRES then
 *    - disable splitting in carrouf
 *    - do a bit of promoting with kthp
 * - When things settle down (tlb_metric < THRES)
 *   - disable kthp to avoid overhead
 *   - do NOT reenable splitting in carrouf (to avoid ping pong effects)
 *
 * Currently I ignore the pgflt/contention metric. It is maybe a bad idea.
 * I also do not take locality / possible NUMA effects into account. Maybe also a bad idea.
 */
   if(enabled_because_tlb) {
      decided_alloc_huge = 1;
      decided_kthp_enabled = 1;
   }

#if ENABLE_LOCK_CONT_METRIC
   // We don't follow the rule because KTHP is too costly
   if((feedback->mmlock + feedback->spinlock) >= LOCK_CONT_THRESHOLD) {
      decided_alloc_huge = 1;
      decided_kthp_enabled = 0;
   }
#endif

#if ENABLE_PGFLT_METRIC
   if(((int) feedback->pgflt - (int) feedback->migrations) >= PGFLT_METRIC_THRES) {
      decided_alloc_huge = 1;
      decided_kthp_enabled = 0;
   }
#endif

#if KEEP_DECISION_FOR_X_TIMES == -1
   if(!current_alloc_huge)
      current_alloc_huge = decided_alloc_huge;

   if(!current_kthp_enabled)
      current_kthp_enabled = decided_kthp_enabled;

#elif KEEP_DECISION_FOR_X_TIMES == 0
   current_alloc_huge = decided_alloc_huge;
   current_kthp_enabled = decided_kthp_enabled;

#else
   if(decided_alloc_huge) {
      if(!current_alloc_huge)
         current_alloc_huge = decided_alloc_huge;
      
      if(!current_kthp_enabled)
         current_kthp_enabled = decided_kthp_enabled;

      last_iteration_valid = 0;
   }
   else {
      if(current_alloc_huge) {
         last_iteration_valid ++;
      }
      
      if(last_iteration_valid == KEEP_DECISION_FOR_X_TIMES) {
         current_kthp_enabled = 0;
         current_alloc_huge = 0;

         last_iteration_valid = 0;
      }
   }
#endif

#if ENABLE_IBS_ASSIST
   // NUMA effect predicted -- Make sure that no previous decisions are applied
   if(!ibs_decision_feedback) {
      alloc_huge = 0;
      kthp_enabled = 0;
   }
#endif

   printf("[DECISION] (K|NA)THP Enabled: %d, Alloc huge: %d, Regular algorithm: %d\n\n", current_kthp_enabled,  current_alloc_huge, DEFAULT_USE_KTHP);

   if(enable_hybrid) {
      if(current_alloc_huge || current_kthp_enabled) {
         write_file("/sys/kernel/mm/transparent_hugepage/enabled", "always"); 

         if(current_kthp_enabled)
            write_file("/sys/kernel/debug/nathp/kthp_enabled", "1");

         hugepages_are_enabled = 1;
      }
   }
   else if(enable_kthp) {
      write_file("/sys/kernel/debug/nathp/alloc_huge", current_alloc_huge?"1":"0");
      write_file("/sys/kernel/debug/nathp/kthp_enabled", current_kthp_enabled?"1":"0");
   }

   /*char *thres = NULL;
   asprintf(&thres, "%d", v->thp_threshold);
   write_file("/sys/kernel/debug/nathp/node_threshold", thres);
   free(thres);*/
}

static inline double _stddev_percent(double array[], int length) {
   double mean;
   double stddev;
   
   mean = gsl_stats_mean(array, 1, length);
   stddev = gsl_stats_sd_m(array, 1, length, mean);

   if(mean) {
      stddev /= mean;
   }
   else {
      stddev = 0;
   }
   
   return stddev*100.;
}

static long percent_running(struct perf_read_ev *last, struct perf_read_ev *prev) {
   long time_enabled = last->time_enabled-prev->time_enabled;
   long time_running = last->time_running-prev->time_running;

   long percent_running = time_enabled ? (100*time_running)/time_enabled:0;
   return percent_running;
}

static void sum_per_core(struct perf_read_ev *last, struct perf_read_ev *prev, unsigned long * per_core_sum) {
   int core;

   for(core = 0; core < nb_cores; core++) {
      int i;
      for(i = 0; i < nb_events_per_core; i++) {
         long idx = core*nb_events_per_core+i;
         long percent_running_counter = percent_running(&last[idx], &prev[idx]);
      
         unsigned long value = last[idx].value - prev[idx].value;

         if(percent_running_counter) {
            value = (value *100) / percent_running_counter;
         }
   
         per_core_sum[i] += value;
      }   
   }
}

static void dram_accesses(struct perf_read_ev *last, struct perf_read_ev *prev, struct hwc_feedback_t * hwc_feedback) {
   int node;
   unsigned long la_global = 0;
   unsigned long ta_global = 0;

   for(node = 0; node < nb_nodes; node++) {
      long node0_idx = node*nb_events_per_node;

      int to_node = 0;
      unsigned long ta = 0;
      unsigned long la = 0;

#if ENABLE_MULTIPLEXING_CHECKS
      long percent_running_n0 = percent_running(&last[node0_idx], &prev[node0_idx]);
#endif

      for(to_node = 0; to_node < nb_nodes; to_node++) { //Hard coded for now. Todo.
         long percent_running_node = percent_running(&last[node0_idx + to_node], &prev[node0_idx + to_node]);

#if ENABLE_MULTIPLEXING_CHECKS
         if(percent_running_node< MIN_ACTIVE_PERCENTAGE) {
            printf("WARNING: %ld %%\n", percent_running_node);
         }

         if(percent_running_node > percent_running_n0+1 || percent_running_node < percent_running_n0-1) { //Allow 1% difference
            printf("WARNING: %% node %d = %ld , %% n0 = %ld\n", to_node, percent_running_node, percent_running_n0);
         }
#endif

         unsigned long da = last[node0_idx + to_node].value - prev[node0_idx + to_node].value;
         if(percent_running_node) {
            da = (da * 100) / percent_running_node; // Try to fix perf mutliplexing issues
         }
         else {
            da = 0;
         }

         //printf("Node %d to node %d : da = %lu, %% running = %ld\n", node, to_node, da, percent_running_node);

         if(node == to_node) {
            la_global += da;
            la += da;
         }

         ta_global += da;
         ta += da;

         hwc_feedback->aggregate_dram_accesses_to_node[to_node] += da;
      }


      if(ta) {
         hwc_feedback->lar_node[node] = ((double) la / (double) ta) * 100.;
      }
   }

   for(node = 0; node < nb_nodes; node++) {
      hwc_feedback->maptu_nodes[node] = 0;
      if(last->time_enabled-prev->time_enabled) {
         hwc_feedback->maptu_nodes[node] = ((double) hwc_feedback->aggregate_dram_accesses_to_node[node] / (double) (last->time_enabled-prev->time_enabled)) * 1000.;
      }

      global_stats.aggregate_dram_accesses_to_node[node] += hwc_feedback->aggregate_dram_accesses_to_node[node];
   }

   if(ta_global) {
      hwc_feedback->lar = ((double) la_global / (double) ta_global) * 100.;

      global_stats.total_nr_accesses += ta_global;
      global_stats.total_nr_local_accesses += la_global;
   }
   else {
      hwc_feedback->lar = 0;
   }

   hwc_feedback->maptu = 0;
   if(last->time_enabled-prev->time_enabled) {
      hwc_feedback->maptu = ((double) ta_global / (double) (last->time_enabled-prev->time_enabled) / (double) (nb_nodes)) * 1000.;
   }

   hwc_feedback->imbalance = _stddev_percent(hwc_feedback->aggregate_dram_accesses_to_node, nb_nodes);
}

/** For now we take decision with a global overview... */
static void apply_carrefour_decisions(struct decision_t* decisions, struct hwc_feedback_t* hwc_feedback) {
   char feedback[MAX_FEEDBACK_LENGTH];

   if(decisions->enable_replication) {
      change_carrefour_state('R');
   }
   else {
      change_carrefour_state('r');
   }
      
   if(decisions->enable_interleaving) {
      int node, written;

      change_carrefour_state('I');
      
      memset(feedback, 0, MAX_FEEDBACK_LENGTH*sizeof(char));

      for(node = 0; node < nb_nodes; node++) {
         if(node == 0) {
            written = snprintf(feedback, MAX_FEEDBACK_LENGTH, "T%lu", (unsigned long) hwc_feedback->aggregate_dram_accesses_to_node[node]);
         }
         else {
            written += snprintf(feedback+written, MAX_FEEDBACK_LENGTH - written, ",%lu", (unsigned long) hwc_feedback->aggregate_dram_accesses_to_node[node]);
         }
      }

      if(written < MAX_FEEDBACK_LENGTH) {
         change_carrefour_state_str(feedback);
      }
      else {
         printf("WARNING: You MUST increase MAX_FEEDBACK_LENGTH!\n");
      }
   }
   else {
      change_carrefour_state('i');
   }

   if(decisions->enable_migration) {
      change_carrefour_state('M');
   }
   else {
      change_carrefour_state('m');
   }

   if(decisions->enable_splitting) {
      change_carrefour_state('S');
   }
   else {
      change_carrefour_state('s');
   }
 
   // send some informations even if carrefour is disabled to split pages
   memset(feedback, 0, MAX_FEEDBACK_LENGTH*sizeof(char));
   snprintf(feedback, MAX_FEEDBACK_LENGTH, "A%lu", (long unsigned) hwc_feedback->maptu);
   change_carrefour_state_str(feedback);

   if(decisions->enable_carrefour) {
      change_carrefour_state('e'); // End profiling + lauches carrefour
      change_carrefour_state('b'); // Start the profiling again
   }
   else {
      change_carrefour_state('x');
   }
}

static struct decision_t carrefour(struct hwc_feedback_t* hwc_feedback,  struct module_feedback_t* module_feedback, int was_efficient) {
   struct decision_t decisions;

   memset(&decisions, 0, sizeof(struct decision_t));

   if (enable_carrefour && hwc_feedback->maptu >= MAPTU_MIN) {
      decisions.enable_carrefour = 1;
   }

   if(decisions.enable_carrefour) {
      /** Check for replication thresholds **/
      decisions.enable_replication = 0;

      /** Check for interleaving threasholds **/
      decisions.enable_interleaving = hwc_feedback->lar < MAX_LOCALITY && hwc_feedback->imbalance > MIN_IMBALANCE;

      /** Check for migration threasholds **/
      decisions.enable_migration = hwc_feedback->lar < MAX_LOCALITY_MIGRATION;

      /** Check for split threshold **/
      decisions.enable_splitting = (hwc_feedback->maptu >= (MAPTU_MIN * 6)) && (decisions.enable_interleaving || decisions.enable_migration || nr_thp_split) ;
   }

#if EVALUATE_EFFICIENCY_STRONG
   if(was_efficient != 1) {
      // Try to find the one that is responsible and disable it for the next iteration

      if(current_decisions.enable_replication) {
         decisions.enable_replication = 0;
      }

      if(current_decisions.enable_interleaving) {
         decisions.enable_interleaving = 0;
      }

      if(current_decisions.enable_migration) {
         decisions.enable_migration = 0;
      }

      if(module_feedback->nr_thp_split) {
         decisions.enable_splitting = 0;

         if(was_efficient == -1) {
            write_file("/sys/kernel/mm/transparent_hugepage/enabled", "always"); 
            hugepages_are_enabled = 1;
         }
      }
   }
#endif

   /** Update state **/
   if(!decisions.enable_replication && !decisions.enable_interleaving && !decisions.enable_migration && !decisions.enable_splitting) {
      decisions.enable_carrefour = 0;
   }

   printf("[DECISION] Carrefour %s, migration %s, interleaving %s, replication %s, splitting %s\n\n",
         decisions.enable_carrefour    ? "Enabled" : "Disabled",
         decisions.enable_migration    ? "Enabled" : "Disabled",
         decisions.enable_interleaving ? "Enabled" : "Disabled",
         decisions.enable_replication  ? "Enabled" : "Disabled",
         decisions.enable_splitting    ? "Enabled" : "Disabled");

   return decisions;
}

double elapsedTime;

static inline void fill_struct(char * file, unsigned long * feedback, size_t nb_elems) {
   FILE *procs = fopen(file, "r");
   int i = 0;

   memset(feedback, 0, sizeof(unsigned long) * nb_elems);

   if(!procs) {
      printf("Cannot open file %s", file);
      return;
   }
   
   for(i = 0; i < nb_elems; i++) {
      if(!fscanf(procs, "%lu", &feedback[i])) {
         printf("[WARNING] Cannot retrieve field number %d\n", i);
         break;
      }
   }

   fclose(procs);
}


static inline void get_kernel_feedback(struct kernel_feedback_t* feedback) {
   fill_struct("/proc/time_lock", (unsigned long *) feedback, sizeof(struct kernel_feedback_t) / sizeof(unsigned long));
}

static inline void get_split_decision(struct module_feedback_t* feedback) {
   fill_struct("/proc/inter_cntl", (unsigned long *) feedback, sizeof(struct module_feedback_t) / sizeof(unsigned long));
}

static inline int get_ibs_decision(double hwc_lar) {
   FILE *procs = fopen("/proc/inter_cntl", "r");
   
   char *line = NULL;
   size_t len = 0;
   ssize_t read;
   int offset = 0;

   long ibs_lar_4k;
   long ibs_lar_2M;

   double* ibs_nr_accesses_4k = malloc(nb_nodes * sizeof(double));
   double* ibs_nr_accesses_2M = malloc(nb_nodes * sizeof(double));

   double imbalance_4k, imbalance_2M;

   int decision = 1;
   int i;

   if(!procs) {
      printf("Cannot open file /proc/inter_cntl\n");
      exit(EXIT_FAILURE);
   }

   read = getline(&line, &len, procs);
   if(read == -1) {
      printf("Malformed IBS feedback !\n");
      exit(EXIT_FAILURE);
   }

   if(sscanf(line, "%lu %lu", &ibs_lar_4k, &ibs_lar_2M) != 2) {
      printf("Malformed IBS feedback !\n");
      exit(EXIT_FAILURE);
   }

   read = getline(&line, &len, procs);
   if(read == -1) {
      printf("Malformed IBS feedback !\n");
      exit(EXIT_FAILURE);
   }

   for(i = 0; i < nb_nodes; i++) {
      if(sscanf(line, "%lf%n", &ibs_nr_accesses_4k[i], &offset) != 1) {
         printf("Cannot get IBS feedback !\n");
         exit(EXIT_FAILURE);
      }
      line += offset;
   }

   read = getline(&line, &len, procs);
   if(read == -1) {
      printf("Malformed IBS feedback !\n");
      exit(EXIT_FAILURE);
   }

   offset = 0;
   for(i = 0; i < nb_nodes; i++) {
      if(sscanf(line, "%lf%n", &ibs_nr_accesses_2M[i], &offset) != 1) {
         printf("Cannot get IBS feedback !\n");
         exit(EXIT_FAILURE);
      }
      line += offset;
   }

   fclose(procs);

   imbalance_4k = _stddev_percent((double*) ibs_nr_accesses_4k, nb_nodes);
   imbalance_2M = _stddev_percent((double*) ibs_nr_accesses_2M, nb_nodes);

   
   //if(ibs_lar_2M < (ibs_lar_4k - 5)) {
   if(ibs_lar_2M < (hwc_lar - 5)) {
      decision = 0;
   }

   if(imbalance_2M > (imbalance_4k + 5)) {
      decision = 0;
   }

   printf("[ %4.3f ] LAR 4k = %ld %%, LAR 2M = %ld %%, HWC LAR = %.0lf, IMB 4k = %.0lf %%, IMB 2M = %.0lf %% -- decision = %d\n", elapsedTime, ibs_lar_4k, ibs_lar_2M, hwc_lar, imbalance_4k, imbalance_2M, decision);
   free(ibs_nr_accesses_4k);
   free(ibs_nr_accesses_2M);
   return decision;
}


static inline int get_nr_thp() {
   FILE *procs = fopen("/proc/vmstat", "r");
   char *line = NULL;
   size_t len = 0;
   ssize_t read;

   int nr_thp = 0;

   if(!procs) {
      printf("Cannot open file /proc/vmstat\n");
      exit(EXIT_FAILURE);
   }

   while ((read = getline(&line, &len, procs)) != -1) {
      if(fscanf(procs, "nr_anon_transparent_hugepages %d\n", &nr_thp)) {
         break;
      }
   }

   fclose(procs);
   return nr_thp;
}

#if EVALUATE_EFFICIENCY
static inline int was_efficient (struct hwc_feedback_t * previous, struct hwc_feedback_t * current) {
   if(current->lar > (previous->lar * (100. + EFFICIENCY_THRESHOLD) / 100.)) {
      return 1; // Lar improved -- Means that carrefour was efficient
   }
   else if (current->lar < (previous->lar * (100. - 5) / 100.)) {
      return -1;
   }

   if(current->imbalance < (previous->imbalance * (100. - EFFICIENCY_THRESHOLD) / 100.)) {
      return 1; // Imbalance improved -- Means that carrefour was efficient
   }
   else if (current->lar < (previous->lar * (100. - 5) / 100.)) {
      return -1;
   }

   // Nothing inproved -- Does not mean anything because there could have been a phase change
   // Consider everything as inefficient for now
   return 0;
}
#else
static inline int was_efficient (struct hwc_feedback_t * previous, struct hwc_feedback_t * current) { return 1; }
#endif

static void thread_loop() {
   int i, j;
   int *fd_per_core = calloc(nb_events_per_core * sizeof(*fd_per_core) * nb_cores, 1);
   int *fd_per_node = calloc(nb_events_per_node * sizeof(*fd_per_node) * nb_nodes, 1);

   struct perf_event_attr *events_attr_per_core = calloc(nb_events_per_core * sizeof(*events_attr_per_core) * nb_cores, 1);
   struct perf_event_attr *events_attr_per_node = calloc(nb_events_per_node * sizeof(*events_attr_per_node) * nb_nodes, 1);

   struct timeval t1, t2;
   struct timeval last_time_carrefour, last_time_kthp, last_time_profiling;

   int sleep_time_carrefour = sleep_time_carrefour_efficient;
   
   int carrefour_was_enabled = 0;
   int carrefour_has_been_enabled_once = 0;

   unsigned logical_time = 0;

   assert(events_attr_per_node && events_attr_per_core);
   assert(fd_per_node && fd_per_core);

   memset(&global_stats, 0, sizeof(struct global_stats_t));

   // First register per node events
   for(i = 0; i < nb_nodes; i++) {
      int core = cpu_of_node(i);
      for (j = 0; j < nb_events_per_node; j++) {
         if(!(per_node_events[j].used_by & USED_BY_KTHP || per_node_events[j].used_by & USED_BY_CARREFOUR)) {
            printf("Line %d: Big BUG\n", __LINE__);
            exit(-1);
         }

         //printf("Registering event %d on node %d\n", j, i);
         events_attr_per_node[i*nb_events_per_node + j].size = sizeof(struct perf_event_attr);
         events_attr_per_node[i*nb_events_per_node + j].type = per_node_events[j].type;
         events_attr_per_node[i*nb_events_per_node + j].config = per_node_events[j].config;
         events_attr_per_node[i*nb_events_per_node + j].exclude_kernel = per_node_events[j].exclude_kernel;
         events_attr_per_node[i*nb_events_per_node + j].exclude_user = per_node_events[j].exclude_user;
         events_attr_per_node[i*nb_events_per_node + j].read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
         fd_per_node[i*nb_events_per_node + j] = sys_perf_counter_open(&events_attr_per_node[i*nb_events_per_node + j], -1, core, (per_node_events[j].leader==-1)?-1:fd_per_node[i*nb_events_per_node + per_node_events[j].leader], 0);

         if (fd_per_node[i*nb_events_per_node + j] < 0) {
            printf("#[%d] sys_perf_counter_open failed: %s\n", core, strerror(errno));
            return;
         }
      }
   }
   
   // Then register per core events
   for(i = 0; i < nb_cores; i++) {
      for (j = 0; j < nb_events_per_core; j++) {
         if(!(per_core_events[j].used_by & USED_BY_KTHP || per_core_events[j].used_by & USED_BY_CARREFOUR)) {
            printf("Line %d: Big BUG\n", __LINE__);
            exit(-1);
         }

         //printf("Registering event %d on node %d\n", j, i);
         events_attr_per_core[i*nb_events_per_core + j].size = sizeof(struct perf_event_attr);
         events_attr_per_core[i*nb_events_per_core + j].type = per_core_events[j].type;
         events_attr_per_core[i*nb_events_per_core + j].config = per_core_events[j].config;
         events_attr_per_core[i*nb_events_per_core + j].exclude_kernel = per_core_events[j].exclude_kernel;
         events_attr_per_core[i*nb_events_per_core + j].exclude_user = per_core_events[j].exclude_user;
         events_attr_per_core[i*nb_events_per_core + j].read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
         fd_per_core[i*nb_events_per_core + j] = sys_perf_counter_open(&events_attr_per_core[i*nb_events_per_core + j], -1, i, (per_core_events[j].leader==-1)?-1:fd_per_core[i*nb_events_per_core + per_core_events[j].leader], 0);

         if (fd_per_core[i*nb_events_per_core + j] < 0) {
            printf("#[%d] sys_perf_counter_open failed: %s\n", i, strerror(errno));
            return;
         }
      }
   }

   struct perf_read_ev single_count;

   struct perf_read_ev *last_counts_per_node = calloc(nb_nodes*nb_events_per_node, sizeof(*last_counts_per_node));
   struct perf_read_ev *last_counts_per_node_for_dump = calloc(nb_nodes*nb_events_per_node, sizeof(*last_counts_per_node_for_dump));
   struct perf_read_ev *last_counts_prev_per_node = calloc(nb_nodes*nb_events_per_node, sizeof(*last_counts_prev_per_node));

   struct perf_read_ev *last_counts_per_core = calloc(nb_cores*nb_events_per_core, sizeof(*last_counts_per_core));
   struct perf_read_ev *last_counts_per_core_for_dump = calloc(nb_cores*nb_events_per_core, sizeof(*last_counts_per_core_for_dump));
   struct perf_read_ev *last_counts_prev_per_core = calloc(nb_cores*nb_events_per_core, sizeof(*last_counts_prev_per_core));

   unsigned long *per_core_sum;

   int carrefour_loaded = is_carrefour_loaded();
   if(!carrefour_loaded) {
      printf("Carrefour module is not present. Decisions won't be applied\n");
      if(enable_carrefour) {
         printf("That's a BUG, you must load carrefour module first !\n");
      }
   }
   else {
      change_carrefour_state('b'); // Make sure that the profiling is started
   }

   memset(&previous_hwc_feedback, 0, sizeof(struct hwc_feedback_t));
   per_core_sum = (unsigned long*) malloc(nb_events_per_core * sizeof(unsigned long));

   memset(&current_decisions, 0, sizeof(struct decision_t));

   gettimeofday(&t1, NULL);
   last_time_carrefour = t1;
   last_time_kthp = t1;
   last_time_profiling = t1;

   while (1) {
      struct hwc_feedback_t hwc_feedback;

      double tlb_metric_value = 0;
      int tlb_metric_index = 0;
      unsigned long nr_pgflt = 0;

      int ibs_decision_feedback;
      int nr_thp;
      
      struct sysinfo info;
      struct kernel_feedback_t feedback;

      int should_enable_carrefour, should_enable_kthp, should_enable_profiling; 
      int sleep_time = -1;
      int sleep_time_default = (sleep_time_carrefour > sleep_time_kthp) ? sleep_time_carrefour : sleep_time_kthp;
      
      unsigned long ttime;

      struct module_feedback_t mf;

      int we = 1;

      gettimeofday(&t2, NULL);

      if(enable_kthp || enable_hybrid) {
         sleep_time = sleep_time_kthp;
      }

      sleep_time = ((sleep_time == -1) || sleep_time_carrefour < sleep_time) ? sleep_time_carrefour : sleep_time;
      
      write_file_ulong("/sys/kernel/debug/carrefour/iteration_length_cycles", (long unsigned) sleep_time * clk_speed / TIME_SECOND);
      //write_file_ulong("/sys/kernel/debug/carrefour/iteration_length_cycles", (long unsigned) sleep_time_carrefour_efficient * clk_speed / TIME_SECOND);

      usleep(sleep_time);
      
      gettimeofday(&t2, NULL);
      should_enable_kthp = (enable_kthp || enable_hybrid) && (TIME_DIFF_US(t2, last_time_kthp) > sleep_time_kthp);
      should_enable_carrefour = (TIME_DIFF_US(t2, last_time_carrefour) > sleep_time_carrefour);
      should_enable_profiling = (TIME_DIFF_US(t2, last_time_profiling) > sleep_time_default);

      //printf("[ %4.3f ] Should enable carrefour: %d \n", TIME_DIFF_S(t2,t1), should_enable_carrefour);
      //printf("[ %4.3f ] Should enable kthp: %d\n", TIME_DIFF_S(t2, t1), should_enable_kthp);
      
      if(should_enable_carrefour) {
         last_time_carrefour = t2;
      }
         
      if(should_enable_kthp) {
         last_time_kthp = t2;
      }

      if(should_enable_profiling) {
         last_time_profiling = t2;
      }

      rdtscll(ttime);

      // First read per node counters
      for(i = 0; i < nb_nodes; i++) {
         for (j = 0; j < nb_events_per_node; j++) {
            assert(read(fd_per_node[i*nb_events_per_node + j], &single_count, sizeof(single_count)) == sizeof(single_count));

            if(!quiet) {
               //#Event   Core  Time        Samples  % time enabled logical time
               fprintf(stderr, "%d\t%d\t%lu\t%lu\t%.3f\t%d\n",
                     per_node_events[j].global_event_no, cpu_of_node(i), ttime, single_count.value - last_counts_per_node_for_dump[i*nb_events_per_node + j].value,
                     (single_count.time_enabled-last_counts_per_node_for_dump[i*nb_events_per_node + j].time_enabled)?((double)(single_count.time_running-last_counts_per_node_for_dump[i*nb_events_per_node + j].time_running))/((double)(single_count.time_enabled-last_counts_per_node_for_dump[i*nb_events_per_node + j].time_enabled)):0,
                     logical_time);
               last_counts_per_node_for_dump[i*nb_events_per_node + j] = single_count;
            }

            if(should_enable_profiling || (should_enable_carrefour && (per_node_events[j].used_by & USED_BY_CARREFOUR)) || (should_enable_kthp && (per_node_events[j].used_by & USED_BY_KTHP))) {
               last_counts_per_node[i*nb_events_per_node + j] = single_count;
            }
            else {
               // Because we don't fill all the entries, make sure that we copy the old one
               last_counts_per_node[i*nb_events_per_node + j] = last_counts_prev_per_node[i*nb_events_per_node + j];
            }
         }
      }

      // Then read per core counters
      for(i = 0; i < nb_cores; i++) {
         for (j = 0; j < nb_events_per_core; j++) {
            assert(read(fd_per_core[i*nb_events_per_core + j], &single_count, sizeof(single_count)) == sizeof(single_count));
      
            if(!quiet) {
               //#Event   Core  Time        Samples  % time enabled logical time
               fprintf(stderr, "%d\t%d\t%lu\t%lu\t%.3f\t%d\n",
                     per_core_events[j].global_event_no, i, ttime, single_count.value - last_counts_per_core_for_dump[i*nb_events_per_core + j].value,
                     (single_count.time_enabled-last_counts_per_core_for_dump[i*nb_events_per_core + j].time_enabled)?((double)(single_count.time_running-last_counts_per_core_for_dump[i*nb_events_per_core + j].time_running))/((double)(single_count.time_enabled-last_counts_per_core_for_dump[i*nb_events_per_core + j].time_enabled)):0,
                     logical_time);
               last_counts_per_core_for_dump[i*nb_events_per_core + j] = single_count;
            }

            if(should_enable_profiling || (should_enable_carrefour && (per_core_events[j].used_by & USED_BY_CARREFOUR)) || (should_enable_kthp && (per_core_events[j].used_by & USED_BY_KTHP))) {
               last_counts_per_core[i*nb_events_per_core + j] = single_count;
            }
            else {
               last_counts_per_core[i*nb_events_per_core + j] = last_counts_prev_per_core[i*nb_events_per_core + j];
            }
         }
      }

      if(should_enable_carrefour || should_enable_profiling) {
         memset(&hwc_feedback, 0, sizeof(struct hwc_feedback_t));
         dram_accesses(last_counts_per_node, last_counts_prev_per_node, &hwc_feedback);
         
         if (sysinfo(&info) != 0) {
            printf("sysinfo: error reading system statistics");
            hwc_feedback.mem_usage = 0;
         }
         else {
            hwc_feedback.mem_usage =  (double) (info.totalram-info.freeram) / (double) info.totalram * 100.;
         }
      }

      memset(per_core_sum, 0, nb_events_per_core * sizeof(unsigned long));
      sum_per_core(last_counts_per_core, last_counts_prev_per_core, per_core_sum);

      if(hugepages_are_enabled) {
         should_enable_kthp = 0;
      }

      if(should_enable_kthp || should_enable_profiling) {
         get_kernel_feedback(&feedback);
         nr_thp = get_nr_thp();
         nr_pgflt = per_core_sum[tlb_metric_index+2]; 

         if(per_core_sum[tlb_metric_index + 1]) {
            tlb_metric_value = (100. * per_core_sum[tlb_metric_index]) / (double) per_core_sum[tlb_metric_index + 1];

            global_stats.total_nr_L2_misses_pgtblewalk += per_core_sum[tlb_metric_index];
            global_stats.total_nr_L2_misses += per_core_sum[tlb_metric_index + 1];
         }
      }

      gettimeofday(&t2, NULL);
      elapsedTime = TIME_DIFF_S(t2, t1);
      
      if(should_enable_profiling || should_enable_carrefour) {
         for(i = 0; i < nb_nodes; i++) {
            printf("[ %3.3f ] [ Node %d ] MAPTU = %.1f - # of accesses = %.1f - LAR = %.1f\n",
                  elapsedTime, i, hwc_feedback.maptu_nodes[i], hwc_feedback.aggregate_dram_accesses_to_node[i], hwc_feedback.lar_node[i]);
         }
         printf("[ %4.3f ] %.1f %% read accesses - MAPTU = %.1f - LAR = %.1f - Imbalance = %.1f %% - Mem usage = %.1f %%\n",
               elapsedTime, hwc_feedback.rr, hwc_feedback.maptu, hwc_feedback.lar, hwc_feedback.imbalance, hwc_feedback.mem_usage);
      }

      if(should_enable_profiling || should_enable_kthp) {
         printf("[ %4.3f ] TLB metric = %.1f %% - # pgflt = %lu ( %lu %% ) - mm lock %lu %% - spinlock %lu %% - mmap %lu %% - brk %lu %% - munmap %lu %% - mprotect %lu %% - migrations %lu %% - nr thp %d\n",
               elapsedTime, tlb_metric_value, nr_pgflt, feedback.pgflt, 
               feedback.mmlock, feedback.spinlock, feedback.mmap, feedback.brk, feedback.munmap, feedback.mprotect, feedback.migrations, nr_thp);
      }

      if(enable_carrefour && should_enable_carrefour && carrefour_has_been_enabled_once) {
         we = was_efficient(&previous_hwc_feedback, &hwc_feedback);

         get_split_decision(&mf);

         nr_thp_split += mf.nr_thp_split;

         if(we != 1 && !mf.split_has_been_chosen) {
            if(carrefour_was_enabled) {
               // The previous carrefour decisions were not efficient
               // Let's be more careful !
               should_enable_carrefour = 0;
               change_carrefour_state('x');

               carrefour_was_enabled = 0;
            }
            sleep_time_carrefour = sleep_time_carrefour_not_efficient;
         }
         else {
            sleep_time_carrefour = sleep_time_carrefour_efficient;
         }
      }

      if(carrefour_loaded && should_enable_carrefour) {
         struct decision_t decisions = carrefour(&hwc_feedback, &mf, we);
         apply_carrefour_decisions(&decisions, &hwc_feedback);

         carrefour_has_been_enabled_once = 1;
         carrefour_was_enabled = 1;

         get_split_decision(&mf);

         if(mf.split_has_been_chosen) {
            sleep_time_carrefour = sleep_time_carrefour_efficient;;
         }

         if(mf.thp_state != 1) {
            hugepages_are_enabled = 0;
         }

         memcpy(&current_decisions, &decisions, sizeof(struct decision_t));
      }

      if(should_enable_kthp) {
#if ENABLE_IBS_ASSIST
         ibs_decision_feedback = get_ibs_decision(lar);
#else
         ibs_decision_feedback = 0;
#endif
         change_nathp_state(tlb_metric_value, &feedback, ibs_decision_feedback);
      }

      memcpy(last_counts_prev_per_node, last_counts_per_node, nb_nodes*nb_events_per_node * sizeof(*last_counts_per_node));
      memcpy(last_counts_prev_per_core, last_counts_per_core, nb_cores*nb_events_per_core * sizeof(*last_counts_per_core));

      memcpy(&previous_hwc_feedback, &hwc_feedback, sizeof(struct hwc_feedback_t));

      logical_time++;
   }

   return;
}


static long sys_perf_counter_open(struct perf_event_attr *hw_event, pid_t pid, int cpu, int group_fd, unsigned long flags) {
   int ret = syscall(__NR_perf_counter_open, hw_event, pid, cpu, group_fd, flags);
#  if defined(__x86_64__) || defined(__i386__)
   if (ret < 0 && ret > -4096) {
      errno = -ret;
      ret = -1;
   }
#  endif
   return ret;
}

static void sig_handler(int signal) {
   printf("#signal caught: %d\n", signal);

   // Stop Carrefour
   change_carrefour_state('k');

   // Reset NATHP state
   if(enable_kthp || enable_hybrid) {
      write_file("/sys/kernel/debug/nathp/enabled", "0");
      write_file("/sys/kernel/debug/nathp/alloc_huge", "0");
      write_file("/sys/kernel/debug/nathp/kthp_enabled", "0");
      write_file("/sys/kernel/mm/transparent_hugepage/enabled", "madvise"); 
   }

   // Print global stats
   printf("[ GLOBAL ] TLB metric = %.1f %% - LAR = %.1f %% - Imbalance = %.1f %%\n",
         global_stats.total_nr_L2_misses ? global_stats.total_nr_L2_misses_pgtblewalk * 100. / global_stats.total_nr_L2_misses : 0,
         global_stats.total_nr_accesses ? global_stats.total_nr_local_accesses * 100. / global_stats.total_nr_accesses : 0,
         _stddev_percent(global_stats.aggregate_dram_accesses_to_node, nb_nodes)
         );

   fflush(NULL);
   exit(0);
}


#include <sched.h>
#include <linux/unistd.h>
#include <sys/mman.h>
static pid_t gettid(void) {
      return syscall(__NR_gettid);
}

void set_affinity(int cpu_id) {
   int tid = gettid();
   cpu_set_t mask;
   CPU_ZERO(&mask);
   CPU_SET(cpu_id, &mask);
   printf("Setting tid %d on core %d\n", tid, cpu_id);
   int r = sched_setaffinity(tid, sizeof(mask), &mask);
   if (r < 0) {
      printf("couldn't set affinity for %d\n", cpu_id);
      exit(1);
   }
}

int main(int argc, char**argv) {
   int i;
   unsigned global_event_no = 0;

   clk_speed = get_cpu_freq();

   signal(SIGPIPE, sig_handler);
   signal(SIGTERM, sig_handler);
   signal(SIGINT, sig_handler);

   set_affinity(0);

   for(i = 1; i < argc; i++) {
      if(!strcmp(argv[i], "--enable-carrefour")) {
         enable_carrefour = 1;
         printf("Enabling carrefour\n");
      }
      else if(!strcmp(argv[i], "--enable-kthp")) {
         enable_kthp = 1;
         printf("Enabling kthp\n");
      }
      else if(!strcmp(argv[i], "--enable-hybrid")) {
         enable_hybrid = 1;
         printf("Enabling hybrid\n");
      }
      else if(!strcmp(argv[i], "-q")) {
         quiet = 1;
      }
      else {
         printf("Unknown arg: %s\n", argv[i]);
         exit(-1);
      }
   }   

   nb_nodes = numa_num_configured_nodes();
   nb_cores = get_nprocs();

   fprintf(stderr, "#NB cpus :\t%d\n", nb_cores);
   fprintf(stderr, "#NB nodes :\t%d\n", nb_nodes);
   for (i = 0; i < nb_nodes; i++) {
      struct bitmask * bm = numa_allocate_cpumask();
      numa_node_to_cpus(i, bm);

      fprintf(stderr, "#Node %d :\t", i);
      int j = 0;
      for (j = 0; j < nb_cores; j++) {
         if (numa_bitmask_isbitset(bm, j)) {
            fprintf(stderr, "%d ", j);
         }
      }
      fprintf(stderr, "\n");
      numa_free_cpumask(bm);
   }
   fprintf(stderr, "#Clock speed: %llu\n", (long long unsigned)clk_speed);

   for(i = 0; i< nb_events_per_node; i++) {
      per_node_events[i].global_event_no = global_event_no++;

      fprintf(stderr, "#Event %d: %s (%llx) (Exclude Kernel: %s; Exclude User: %s, per node)\n", per_node_events[i].global_event_no, per_node_events[i].name, (long long unsigned)per_node_events[i].config, (per_node_events[i].exclude_kernel)?"yes":"no", (per_node_events[i].exclude_user)?"yes":"no");
   }
   for(i = 0; i< nb_events_per_core; i++) {
      per_core_events[i].global_event_no = global_event_no++;

      fprintf(stderr, "#Event %d: %s (%llx) (Exclude Kernel: %s; Exclude User: %s, per core)\n", per_core_events[i].global_event_no, per_core_events[i].name, (long long unsigned)per_core_events[i].config, (per_core_events[i].exclude_kernel)?"yes":"no", (per_core_events[i].exclude_user)?"yes":"no");
   }
   fprintf(stderr, "#Event\tCore\tTime\t\t\tSamples\t%% time enabled\tlogical time\n");

   printf("Parameters :\n");
#if ENABLE_MULTIPLEXING_CHECKS
   printf("\tMIN_ACTIVE_PERCENTAGE = %d\n", MIN_ACTIVE_PERCENTAGE);
#endif
   printf("\tMAPTU_MIN = %d accesses / usec\n", MAPTU_MIN);
   printf("\tMIN_IMBALANCE = %d %%\n", MIN_IMBALANCE);
   printf("\tMAX_LOCALITY = %d %%\n", MAX_LOCALITY);
   printf("\tMAX_LOCALITY_MIGRATION = %d %%\n", MAX_LOCALITY_MIGRATION);
   printf("\tDEFAULT_USE_KTHP = %d\n", DEFAULT_USE_KTHP);
   printf("\tALLOC_HUGE_WHEN_ENABLED = %d\n", ALLOC_HUGE_WHEN_ENABLED);
   printf("\tKEEP_DECISION_FOR_X_TIMES = %d\n", KEEP_DECISION_FOR_X_TIMES);
   printf("\tTLB_METRIC_THRES = %d %%\n", TLB_METRIC_THRES);
   printf("\tENABLE_PGFLT_METRIC = %d\n", ENABLE_PGFLT_METRIC);
   printf("\tPGFLT_METRIC_THRES = %d\n", PGFLT_METRIC_THRES);
   printf("\tENABLE_LOCK_CONT_METRIC = %d\n", ENABLE_LOCK_CONT_METRIC);
   printf("\tLOCK_CONT_THRESHOLD = %d\n", LOCK_CONT_THRESHOLD);

   printf("\tENABLE_IBS_ASSIST = %d\n", ENABLE_IBS_ASSIST);

   printf("\tEVALUATE_EFFICIENCY = %d\n", EVALUATE_EFFICIENCY);
   printf("\tEVALUATE_EFFICIENCY_STRONG = %d\n", EVALUATE_EFFICIENCY_STRONG);
   printf("\tEFFICIENCY_THRESHOLD = %d\n", EFFICIENCY_THRESHOLD);

   if(nb_nodes > NB_NODES) {
      printf("You MUST increase the NB_NODES value (currently set to: %d)!\n", NB_NODES);
      exit(EXIT_FAILURE);
   }


   if(enable_kthp || enable_hybrid) {
      if(enable_kthp) {
         /* Disable by default */
         write_file("/sys/kernel/debug/nathp/alloc_huge", "0");
         write_file("/sys/kernel/debug/nathp/kthp_enabled", "0");
      }
      else {
         /* Enable by default */
         write_file("/sys/kernel/debug/nathp/alloc_huge", "1");
         write_file("/sys/kernel/debug/nathp/kthp_enabled", "0");

         hugepages_are_enabled = 1;
      }

      /** Default params **/
      write_file("/sys/kernel/debug/nathp/enabled", "1");
      write_file("/sys/kernel/mm/transparent_hugepage/enabled", "always"); 
      write_file("/sys/kernel/debug/nathp/regular_algorithm", DEFAULT_USE_KTHP?"1":"0");
   }

   thread_loop();

   return 0;
}

