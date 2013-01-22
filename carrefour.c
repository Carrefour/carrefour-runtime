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

static int sleep_time = 1*TIME_SECOND;     /* Profile by sleep_time useconds chunks */

#define MIN_ACTIVE_PERCENTAGE       15

/* Only triggers carrefour if the rate of memory accesses is above the threshold and the IPC is below the other one */
#define MAPTU_MIN                   50

/* Replication thresholds */
#define MEMORY_USAGE_MAX            25 // The global memory usage must be under XX% to enable replication
#define USE_MRR                     0  // Use MRR or DCMR

#if USE_MRR
#define MRR_MIN                     90 // Enable replication if the memory read ratio is above the threshold
#else
#define DCRM_MAX                    5  // Enable replication if the data cache modified ratio is below X%
#endif

/* Interleaving thresholds */
#define MIN_IMBALANCE               35 /* Deviation in % */
#define MAX_LOCALITY                100 /* In % - We don't want to strongly decrease the locality */

/* Migration threshold */
#define MAX_LOCALITY_MIGRATION      80 /* In % */
/***/

#define ENABLE_MULTIPLEXING_CHECKS  0
#define VERBOSE                     1

/** IPC is now disabled by default **/
#define ENABLE_IPC                  0
#define IPC_MAX                     0.9

/** Internal **/
#define MAX_FEEDBACK_LENGTH         256


#if !VERBOSE
#define printf(args...) do {} while(0)
#endif

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
static event_t default_events[] = {
#if USE_MRR
   /** MRR **/
   {
      .name    = "MRR_READ",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1004062F0,
      .leader  = -1,
   },
   {
      .name    = "MRR_READ_WRITE",
      .type    = PERF_TYPE_RAW,
      .config  = 0x100407BF0,
      .leader  = 0
   },
#else
   /** DCMR */
   {
      .name    = "DCR_ALL",
      .type    = PERF_TYPE_RAW,
      .config  = 0x000401F43,
      .leader  = -1,
   },
   {
      .name    = "DCR_MODIFIED",
      .type    = PERF_TYPE_RAW,
      .config  = 0x000401043,
      .leader  = 0,
   },
#endif

   /** LAR & DRAM imbalance **/
   {
      .name    = "CPU_DRAM_NODE0",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1004001E0,
      .leader  = -1
   },
   {
      .name    = "CPU_DRAM_NODE1",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1004002E0,
      .leader  = -1
      //.leader  = 2
   },
   {
      .name    = "CPU_DRAM_NODE2",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1004004E0,
      .leader  = -1
      //.leader  = 2
   },
   {
      .name    = "CPU_DRAM_NODE3",
      .type    = PERF_TYPE_RAW,
      .config  = 0x1004008E0,
      .leader  = -1
      //.leader  = 2
   },

#if ENABLE_IPC
   /** IPC **/
   {
      .name    = "CPU_CLK_UNHALTED",
      .type    = PERF_TYPE_RAW,
      .config  = 0x00400076,
      .leader  = -1
   },
   {
      .name    = "RETIRED_INSTRUCTIONS",
      .type    = PERF_TYPE_RAW,
      .config  = 0x004000C0,
      .leader  = 6
   },
#endif
};

static int nb_events = sizeof(default_events)/sizeof(*default_events);
static event_t *events = default_events;

static int nb_nodes;
static uint64_t get_cpu_freq(void) {
   FILE *fd;
   uint64_t freq = 0;
   float freqf = 0;
   char *line = NULL;
   size_t len = 0;

   fd = fopen("/proc/cpuinfo", "r");
   if (!fd) {
      fprintf(stderr, "failed to get cpu frequency\n");
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
  int ncpus, cpu;

  ncpus = numa_num_configured_cpus();
  bmp = numa_bitmask_alloc(ncpus);
  numa_node_to_cpus(node, bmp);
  for(cpu = 0; cpu < ncpus; cpu++) {
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
         fprintf(stderr, "Cannot open the carrefour file. Is carrefour loaded?\n");
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
      fprintf(stderr, "Cannot open the carrefour file. Is carrefour loaded?\n");
   }
}

static long percent_running(struct perf_read_ev *last, struct perf_read_ev *prev) {
   long percent_running = (last->time_enabled-prev->time_enabled)?100*(last->time_running-prev->time_running)/(last->time_enabled-prev->time_enabled):0;
   return percent_running;
}

#if USE_MRR
static void mrr(struct perf_read_ev *last, struct perf_read_ev *prev, double * rr_global, double * maptu_global, double * rr_nodes, double * maptu_nodes) {
   int node;
   unsigned long read_global = 0;
   unsigned long rw_global = 0;

   //unsigned long time_enabled = last->time_enabled-prev->time_enabled;
   unsigned long time_enabled = last->time_running-prev->time_running;

   for(node = 0; node < nb_nodes; node++) {
      long read_idx = node*nb_events;

#if ENABLE_MULTIPLEXING_CHECKS
      long percent_running_read = percent_running(&last[read_idx], &prev[read_idx]);
      long percent_running_rw = percent_running(&last[read_idx + 1], &prev[read_idx + 1]);

      if(percent_running_read < MIN_ACTIVE_PERCENTAGE) {
         printf("WARNING: %ld %%\n", percent_running_read);
      }

      if(percent_running_rw > percent_running_read+1 || percent_running_rw < percent_running_read-1) { //Allow 1% difference
         printf("WARNING: %% read = %ld , %% rw = %ld\n", percent_running_read, percent_running_rw);
      }
#endif

      //printf("Read = %lu , RW = %lu\n", last[read_idx].value - prev[read_idx].value, last[read_idx + 1].value - prev[read_idx + 1].value);
      unsigned long read = last[read_idx].value - prev[read_idx].value;
      unsigned long rw   = last[read_idx + 1].value - prev[read_idx + 1].value;

      read_global += read;
      rw_global += rw;

      rr_nodes[node] = 1;
      maptu_nodes[node] = 0;

      if(rw) {
         rr_nodes[node] = ((double) read) / ((double) rw) * 100.;
      }

      //printf("%lu - %lu - %lu\n", read, rw, time_enabled);
      if(time_enabled) {
         maptu_nodes[node] = (double) rw / (double) time_enabled;
      }
   }

   *rr_global = 1;
   *maptu_global = 0;

   if(rw_global) {
      *rr_global = ((double) read_global) / ((double) rw_global) * 100.;
   }

   //printf("%lu - %lu - %lu\n", read, rw, time_enabled);
   if(time_enabled) {
      *maptu_global = (double) rw_global / (double) time_enabled;
   }
}

#else
static void dcmr(struct perf_read_ev *last, struct perf_read_ev *prev, double * rr_global, double * rr_nodes) {
   int node;
   unsigned long all_global = 0;
   unsigned long modified_global = 0;

   for(node = 0; node < nb_nodes; node++) {
      long all_idx = node*nb_events;

#if ENABLE_MULTIPLEXING_CHECKS
      long percent_running_all = percent_running(&last[all_idx], &prev[all_idx]);
      long percent_running_modified = percent_running(&last[all_idx + 1], &prev[all_idx + 1]);

      if(percent_running_all < MIN_ACTIVE_PERCENTAGE) {
         printf("WARNING: %ld %%\n", percent_running_read);
      }

      if(percent_running_all > percent_running_modified+1 || percent_running_all < percent_running_modified-1) { //Allow 1% difference
         printf("WARNING: %% all = %ld , %% modified = %ld\n", percent_running_all, percent_running_modified);
      }
#endif

      //printf("Read = %lu , RW = %lu\n", last[all_idx].value - prev[all_idx].value, last[all_idx + 1].value - prev[all_idx + 1].value);
      unsigned long all = last[all_idx].value - prev[all_idx].value;
      unsigned long modified = last[all_idx + 1].value - prev[all_idx + 1].value;

      all_global += all;
      modified_global += modified;

      rr_nodes[node] = 100;
      if(all) {
         //printf("%d : %lu - %lu\n", node, modified, all);
         rr_nodes[node] = (1. - (double) modified / (double) all) * 100.;
      }
   }

   *rr_global = 100;
   if(all_global) {
      *rr_global = (1 - (double) modified_global / (double) all_global) * 100.;
   }
}
#endif

static void dram_accesses(struct perf_read_ev *last, struct perf_read_ev *prev, double * lar, double * load_imbalance, double * aggregate_dram_accesses_to_node, double * lar_node, double * maptu_global, double * maptu_nodes) {
   int node;
   unsigned long la_global = 0;
   unsigned long ta_global = 0;

   for(node = 0; node < nb_nodes; node++) {
      long node0_idx = node*nb_events + 2; // The first two events are used to compute the mrr

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

         aggregate_dram_accesses_to_node[to_node] += da;
      }


      if(ta) {
         lar_node[node] = (double) la / (double) ta;
      }
   }

   for(node = 0; node < nb_nodes; node++) {
      if(maptu_nodes) {
         maptu_nodes[node] = 0;
         if(last->time_enabled-prev->time_enabled) {
            maptu_nodes[node] = (double) aggregate_dram_accesses_to_node[node] / (double) (last->time_enabled-prev->time_enabled) ;
         }
      }
   }

   if(ta_global) {
      *lar = (double) la_global / (double) ta_global;
   }
   else {
      *lar = 0;
   }

   if(maptu_global) {
      *maptu_global = 0;
      if(last->time_enabled-prev->time_enabled) {
         *maptu_global = (double) ta_global / (double) (last->time_enabled-prev->time_enabled) ;
      }
   }


   double mean_da = gsl_stats_mean(aggregate_dram_accesses_to_node, 1, nb_nodes);
   double stddev_da = gsl_stats_sd_m(aggregate_dram_accesses_to_node, 1, nb_nodes, mean_da);

   if(mean_da) {
      *load_imbalance = stddev_da / mean_da;
   }
   else {
      *load_imbalance = 0;
   }
}

#if ENABLE_IPC
static void ipc(struct perf_read_ev *last, struct perf_read_ev *prev, double * ipc_global, double * ipc_node) {
   int node;
   unsigned long clk_global = 0;
   unsigned long inst_global = 0;

   for(node = 0; node < nb_nodes; node++) {
      long cpuclock_idx = node*nb_events + 6; // The first two events are used to compute the mrr, the next 4 to compute the LAR

#if ENABLE_MULTIPLEXING_CHECKS
      long percent_running_clock = percent_running(&last[cpuclock_idx], &prev[cpuclock_idx]);
      long percent_running_instructions = percent_running(&last[cpuclock_idx + 1], &prev[cpuclock_idx + 1]);

      if(percent_running_clock < MIN_ACTIVE_PERCENTAGE) {
         printf("WARNING: %ld %%\n", percent_running_clock);
      }

      if(percent_running_clock > percent_running_instructions+1 || percent_running_clock < percent_running_instructions-1) { //Allow 1% difference
         printf("WARNING: %% clock = %ld , %% instructions = %ld\n", percent_running_clock, percent_running_instructions);
      }
#endif

      unsigned long clk = last[cpuclock_idx].value - prev[cpuclock_idx].value;
      unsigned long inst = last[cpuclock_idx + 1].value - prev[cpuclock_idx + 1].value;

      clk_global += clk;
      inst_global += inst;

      if(clk) {
         ipc_node[node] = (double) inst / (double) clk;
      }
      else {
        printf("WARNING: Clk = 0 !!!\n");
      }
   }

   if(clk_global) {
      *ipc_global = (double) inst_global / (double) clk_global;
   }
   else {
      printf("WARNING: clk_global = 0 !!!\n");
   }
}
#endif

/** For now we take decision with a global overview... */
static int carrefour_replication_enabled    = 1; // It is enabled by default
static int carrefour_interleaving_enabled   = 1; // It is enabled by default
static int carrefour_migration_enabled      = 1; // It is enabled by default

#if USE_MRR
static const int rr_min = MRR_MIN;
#else
static const int rr_min = 100 - DCRM_MAX;
#endif

static inline void carrefour(double rr, double maptu, double lar, double imbalance, double *aggregate_dram_accesses_to_node, double ipc, double global_mem_usage) {
   int carrefour_enabled = 0;

#if ENABLE_IPC
   if(maptu >= MAPTU_MIN && ipc <= IPC_MAX) {
      carrefour_enabled = 1;
   }
#else
   if(maptu >= MAPTU_MIN) {
      carrefour_enabled = 1;
   }
#endif

   if(carrefour_enabled) {
      /** Check for replication thresholds **/
      int er = (global_mem_usage <= MEMORY_USAGE_MAX) && (rr >= rr_min);

      if(er && !carrefour_replication_enabled) {
         change_carrefour_state('R');
         carrefour_replication_enabled = 1;
      }
      else if (!er && carrefour_replication_enabled) {
         change_carrefour_state('r');
         carrefour_replication_enabled = 0;
      }

      /** Check for interleaving threasholds **/
      int ei = lar < MAX_LOCALITY && imbalance > MIN_IMBALANCE;

      if(ei && ! carrefour_interleaving_enabled) {
         change_carrefour_state('I');
         carrefour_interleaving_enabled = 1;
      }
      else if(!ei && carrefour_interleaving_enabled) {
         //printf("GLOBAL: disable interleaving (lar = %.1f, imbalance = %.1f)\n", lar, imbalance);
         change_carrefour_state('i');
         carrefour_interleaving_enabled = 0;
      }

      /** Check for migration threasholds **/
      if(lar < MAX_LOCALITY_MIGRATION && ! carrefour_migration_enabled) {
         change_carrefour_state('M');
         carrefour_migration_enabled = 1;
      }
      else if (lar >= MAX_LOCALITY_MIGRATION && carrefour_migration_enabled) {
         change_carrefour_state('m');
         carrefour_migration_enabled = 0;
      }

      /** Interleaving needs more feedback **/
      if(carrefour_interleaving_enabled) {
         char feedback[MAX_FEEDBACK_LENGTH];
         int node, written;
         memset(feedback, 0, MAX_FEEDBACK_LENGTH*sizeof(char));

         for(node = 0; node < nb_nodes; node++) {
            if(node == 0) {
               written = snprintf(feedback, MAX_FEEDBACK_LENGTH, "T%lu", (unsigned long) aggregate_dram_accesses_to_node[node]);
            }
            else {
               written += snprintf(feedback+written, MAX_FEEDBACK_LENGTH - written, ",%lu", (unsigned long) aggregate_dram_accesses_to_node[node]);
            }
         }

         if(written < MAX_FEEDBACK_LENGTH) {
            change_carrefour_state_str(feedback);
         }
         else {
            printf("WARNING: You MUST increase MAX_FEEDBACK_LENGTH!\n");
         }
      }

      /** Update state **/
      if(!carrefour_replication_enabled && !carrefour_interleaving_enabled && !carrefour_migration_enabled) {
         carrefour_enabled = 0;
      }
   }

   printf("[DECISION] Carrefour %s, migration %s, interleaving %s, replication %s\n\n",
         carrefour_enabled ? "Enabled" : "Disabled",
         carrefour_migration_enabled ? "Enabled" : "Disabled",
         carrefour_interleaving_enabled ? "Enabled" : "Disabled",
         carrefour_replication_enabled ? "Enabled" : "Disabled");

   if(carrefour_enabled) {
      change_carrefour_state('e'); // End profiling + lauches carrefour
      change_carrefour_state('b'); // Start the profiling again
   }
   else {
      change_carrefour_state('x');
   }
}

static void thread_loop() {
   int i, j;
   int *fd = calloc(nb_events * sizeof(*fd) * nb_nodes, 1);
   struct perf_event_attr *events_attr = calloc(nb_events * sizeof(*events_attr) * nb_nodes, 1);
   assert(events_attr != NULL);
   assert(fd);
   for(i = 0; i < nb_nodes; i++) {
      int core = cpu_of_node(i);
      for (j = 0; j < nb_events; j++) {
         //printf("Registering event %d on node %d\n", j, i);
         events_attr[i*nb_events + j].size = sizeof(struct perf_event_attr);
         events_attr[i*nb_events + j].type = events[j].type;
         events_attr[i*nb_events + j].config = events[j].config;
         events_attr[i*nb_events + j].exclude_kernel = events[j].exclude_kernel;
         events_attr[i*nb_events + j].exclude_user = events[j].exclude_user;
         events_attr[i*nb_events + j].read_format = PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
         fd[i*nb_events + j] = sys_perf_counter_open(&events_attr[i*nb_events + j], -1, core, (events[j].leader==-1)?-1:fd[i*nb_events + events[j].leader], 0);
         if (fd[i*nb_events + j] < 0) {
            fprintf(stdout, "#[%d] sys_perf_counter_open failed: %s\n", core, strerror(errno));
            return;
         }
      }
   }
	
   struct perf_read_ev single_count;
   struct perf_read_ev *last_counts = calloc(nb_nodes*nb_events, sizeof(*last_counts));
   struct perf_read_ev *last_counts_prev = calloc(nb_nodes*nb_events, sizeof(*last_counts_prev));

   double *rr_nodes, *maptu_nodes;
   double *aggregate_dram_accesses_to_node, *lar_node;
   double * ipc_node;

   rr_nodes = (double *) malloc(nb_nodes*sizeof(double));
   maptu_nodes =  (double *) malloc(nb_nodes*sizeof(double));
   aggregate_dram_accesses_to_node = (double *) malloc(nb_nodes*sizeof(double));
   lar_node = (double *) malloc(nb_nodes*sizeof(double));
   ipc_node = (double *) malloc(nb_nodes*sizeof(double));

   change_carrefour_state('b'); // Make sure that the profiling is started

   while (1) {
      usleep(sleep_time);
      for(i = 0; i < nb_nodes; i++) {
         for (j = 0; j < nb_events; j++) {
            assert(read(fd[i*nb_events + j], &single_count, sizeof(single_count)) == sizeof(single_count));
/*            printf("[%d,%d] %ld enabled %ld running %ld%%\n", i, j,
                  single_count.time_enabled - last_counts[i*nb_events + j].time_enabled,
                  single_count.time_running - last_counts[i*nb_events + j].time_running,
                  (single_count.time_enabled-last_counts[i*nb_events + j].time_enabled)?100*(single_count.time_running-last_counts[i*nb_events + j].time_running)/(single_count.time_enabled-last_counts[i*nb_events + j].time_enabled):0); */
            last_counts[i*nb_events + j] = single_count;
         }
      }

      double rr_global = 0, maptu_global = 0;
      double lar = 0, load_imbalance = 0;
      double ipc_global = 0;

      memset(rr_nodes, 0, nb_nodes*sizeof(double));
      memset(maptu_nodes, 0, nb_nodes*sizeof(double));
      memset(aggregate_dram_accesses_to_node, 0, nb_nodes*sizeof(double));
      memset(lar_node, 0, nb_nodes*sizeof(double));
      memset(ipc_node, 0, nb_nodes*sizeof(double));

#if USE_MRR
      mrr(last_counts, last_counts_prev, &rr_global, &maptu_global, rr_nodes, maptu_nodes);
      dram_accesses(last_counts, last_counts_prev, &lar, &load_imbalance, aggregate_dram_accesses_to_node, lar_node, NULL, NULL);
#else
      dcmr(last_counts, last_counts_prev, &rr_global, rr_nodes);
      dram_accesses(last_counts, last_counts_prev, &lar, &load_imbalance, aggregate_dram_accesses_to_node, lar_node, &maptu_global, maptu_nodes);
#endif


#if ENABLE_IPC
      ipc(last_counts, last_counts_prev, &ipc_global, ipc_node);
#endif

      struct sysinfo info;
      double global_mem_usage = 0;
      if (sysinfo(&info) != 0) {
         printf("sysinfo: error reading system statistics");
         global_mem_usage = 0;
      }
      else {
         global_mem_usage =  (double) (info.totalram-info.freeram) / (double) info.totalram * 100.;
      }


      for(i = 0; i < nb_nodes; i++) {
         printf("[ Node %d ] %.1f %% read accesses - MAPTU = %.1f - # of accesses = %.1f - LAR = %.1f - IPC = %.2f\n",
                  i, rr_nodes[i], maptu_nodes[i] * 1000., aggregate_dram_accesses_to_node[i], lar_node[i] * 100., ipc_node[i]);
      }
      printf("[ GLOBAL ] %.1f %% read accesses - MAPTU = %.1f - LAR = %.1f - Imbalance = %.1f %% - IPC = %.2f - Mem usage = %.1f %%\n",
                  rr_global, maptu_global * 1000., lar * 100., load_imbalance * 100., ipc_global, global_mem_usage);

      carrefour(rr_global, maptu_global * 1000., lar * 100., load_imbalance * 100., aggregate_dram_accesses_to_node, ipc_global, global_mem_usage);

      for(i = 0; i < nb_nodes; i++) {
         for (j = 0; j < nb_events; j++) {
            last_counts_prev[i*nb_events + j] = last_counts[i*nb_events + j];
         }
      }

   }

   free(rr_nodes);
   free(maptu_nodes);
   free(aggregate_dram_accesses_to_node);
   free(lar_node);
   free(ipc_node);

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
   change_carrefour_state('k');
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
      fprintf(stderr, "couldn't set affinity for %d\n", cpu_id);
      exit(1);
   }
}

int main(int argc, char**argv) {
   signal(SIGPIPE, sig_handler);
   signal(SIGTERM, sig_handler);
   signal(SIGINT, sig_handler);

   int i;
   uint64_t clk_speed = get_cpu_freq();

   printf("#Clock speed: %llu\n", (long long unsigned)clk_speed);
   for(i = 0; i< nb_events; i++) {
      printf("#Event %d: %s (%llx) (Exclude Kernel: %s; Exclude User: %s)\n", i, events[i].name, (long long unsigned)events[i].config, (events[i].exclude_kernel)?"yes":"no", (events[i].exclude_user)?"yes":"no");
   }

   printf("Parameters :\n");
   printf("\tMIN_ACTIVE_PERCENTAGE = %d\n", MIN_ACTIVE_PERCENTAGE);
   printf("\tMAPTU_MIN = %d accesses / usec\n", MAPTU_MIN);
   printf("\tMEMORY_USAGE_MAX = %d %%\n", MEMORY_USAGE_MAX);
#if USE_MRR
   printf("\tMRR_MIN = %d %%\n", MRR_MIN);
#else
   printf("\tDCRM_MAX = %d %%\n", DCRM_MAX);
#endif
   printf("\tMIN_IMBALANCE = %d %%\n", MIN_IMBALANCE);
   printf("\tMAX_LOCALITY = %d %%\n", MAX_LOCALITY);
   printf("\tMAX_LOCALITY_MIGRATION = %d %%\n", MAX_LOCALITY_MIGRATION);
#if ENABLE_IPC
   printf("\tIPC_MAX = %f\n", IPC_MAX);
#endif

   nb_nodes = numa_num_configured_nodes();
   thread_loop();

   return 0;
}

