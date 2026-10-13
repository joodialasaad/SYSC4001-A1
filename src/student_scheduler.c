#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <limits.h>

/*
 * This is the source file you must complete.
 *
 * You are responsible for implementing the simulated operating-system
 * behaviour, including:
 *   - PCBs and process states;
 *   - the ready queue and waiting processes;
 *   - CPU and I/O burst accounting;
 *   - FCFS, Round Robin, and preemptive SRTF;
 *   - dispatch/context-switch overhead; and
 *   - calls to the supplied metrics and CSV logging functions.
 *
 * You may replace this file completely and add any .c/.h files you need.
 * The types and helper functions below are only a suggested decomposition.
 * You may rename, replace, combine, or remove them.
 */

/*
 * Suggested PCB starting point. Add the fields required by the handout, such
 * as remaining CPU time, completed bursts, I/O completion time, Ready-entry
 * order, and RR quantum usage.
 */
typedef struct {
    ProcessSpec spec; //specification 
    ProcessState state;
    /* TODO: add the rest of your PCB fields. */
    int remaining_cpu; // -- when process execures a cpu tick
    int completed_bursts; // ++ when a cpu burst ends
    int io_completion_time; // store simulated time when process ends io

    long long ready_order; //when a process enters ready queeue to preserve FIFO 
    int quantum_used; //++ as process uses its RR time slice 
} PCB;

/* You may choose to maintain a structure for your queues.
 * Do so in whichever way you see fit.
 */

/* Keeping whole-simulation state together makes helper signatures smaller. 
 * Store whatever you feel is necessary for your implementation.
 */
typedef struct {
    PCB *pcbs; //array containing every process
    size_t process_count; //n of processes
    int current_time; //simulation clock
    int running_index; //index of current running process (-1 if none is running)
    long long next_ready_order; // counter used to preserve the order of whuch processes enter the ready queue
} Simulation;

/*
 * Supplied output API examples
 * ----------------------------
 * These functions are declared in scheduler.h and implemented in io.c. They
 * format the CSV rows for you and calculate the metrics. You must use them
 * to ensure your output can be correctly processed by the test harness.
 *
 * State transition:
 *
 *   log_transition(output, time, pid,
 *                  STATE_NEW, STATE_READY, REASON_ARRIVAL);
 *
 * One timeline interval [time, time + 1):
 *
 *  If the CPU is running a process:
 *      record_timeline_tick(output, time, ACTIVITY_CPU, running_pid);
 *
 *  If the CPU is performing a context switch:
 *      record_timeline_tick(output, time, ACTIVITY_CONTEXT_SWITCH, 0);
 *
 *  If the CPU is idle:
 *      record_timeline_tick(output, time, ACTIVITY_IDLE, 0);
 *
 * Final metrics (one call after every process terminates):
 *
 *   if (!write_metrics(output, specs, count)) {
 *       return false;
 *   }
 *
 */

/*
 * Suggested helper decomposition (optional)
 * -----------------------------------------
 * You may find helpers with responsibilities like these useful:
 *
 *   initialize_pcbs(...)
 *   ready_enqueue(...)
 *   choose_next_process(...)
 *   admit_arrivals(...)
 *   complete_io(...)
 *   maybe_preempt_srtf(...)
 *   update_dispatch(...)
 *   advance_one_interval(...)
 *
 * Required event-loop order
 * -------------------------
 *
 *   while not all processes are terminated:
 *       resolve the CPU interval that ended at the current time
 *       admit arrivals in ascending PID order
 *       complete I/O in ascending PID order
 *       apply the SRTF preemption rule, if applicable
 *       complete or begin dispatch/context-switch activity
 *       record exactly one CPU, context-switch, or idle timeline tick
 *       advance time by one
 *
 * You must design and implement the loop yourself. The reference outputs care
 * about observable timing and ordering, not the names of your private helpers.
 */
PCB *initialize_pcbs(const ProcessSpec *specs, size_t process_count){
    PCB *pcbs = calloc(process_count, sizeof(PCB));

    if(pcbs == NULL){
        return NULL;
    }

    for(size_t i=0; i < process_count; i++){
        pcbs[i].spec = specs[i]; 
        pcbs[i].state = STATE_NEW;
        pcbs[i].remaining_cpu = specs[i].cpu_burst;
        pcbs[i].completed_bursts = 0;
        pcbs[i].io_completion_time = -1;
        pcbs[i].ready_order = -1;
        pcbs[i].quantum_used = 0;
    }
    return pcbs;
}

void complete_cpu_burst(PCB *pcbs, int running_index, int current_time, OutputContext *output){
    PCB *pcb = &pcbs[running_index];
    pcb->completed_bursts++;

    if(pcb->completed_bursts == pcb->spec.num_cpu_bursts){
        ProcessState old_state = pcb->state;
        pcb->state = STATE_TERMINATED;

        log_transition(output,current_time, pcb->spec.pid, old_state, pcb->state, REASON_PROCESS_COMPLETE);
    
    }
    else {
        ProcessState old_state = pcb->state;
        pcb->state = STATE_WAITING;
        log_transition(output, current_time, pcb->spec.pid, old_state, pcb->state, REASON_CPU_BURST_COMPLETE);
        pcb->io_completion_time = current_time + pcb->spec.io_burst;
    }
}
/*
 * Run one complete scheduling simulation.
 *
 * Parameters
 * ----------
 * specs:
 *     A read-only array of validated process descriptions, sorted by ascending
 *     PID. Each entry supplies the arrival time, CPU- and I/O-burst lengths,
 *     and number of CPU bursts for one process. Copy entries into your PCBs if
 *     you need mutable per-process state. Do not modify or free this array.
 *
 * count:
 *     The number of ProcessSpec entries in specs.
 *
 * config:
 *     A read-only SimulationConfig containing the selected algorithm, the RR
 *     quantum, and the context-switch duration. The quantum affects scheduling
 *     only when config->algorithm is ALG_RR. Do not modify or free config.
 *
 * output:
 *     An already-open output context. Pass it to log_transition(),
 *     record_timeline_tick(), and write_metrics() as described above. main.c
 *     owns this context and will close it; do not close or free it here.
 *
 * Return true after the simulation and final metrics are written successfully.
 * Return false if initialization, simulation, or output generation fails.
 * main.c retains ownership of specs, config, and output in either case.
 */
bool run_simulation(const ProcessSpec *specs,
                    size_t count,
                    const SimulationConfig *config,
                    OutputContext *output)
{
    PCB *pcbs = initialize_pcbs(specs, count);

    if (pcbs == NULL) {
        return false;
    }

    int *ready_queue = malloc(count * sizeof(int));
    if (ready_queue == NULL) {
        free(pcbs);
        return false;
    }

    int current_time = 0;
    int running_index = -1;
    int switch_remaining = 0;
    bool switch_in_progress = false;
    bool dispatch_after_switch = false;

    long long next_ready_order = 0;
    size_t ready_count = 0;
    size_t terminated_count = 0;

    while (terminated_count < count) {

        /*
         * Resolve the CPU interval that ended at current_time.
         */
        if (running_index != -1 &&
            pcbs[running_index].remaining_cpu > 0 &&
            config->algorithm == ALG_RR &&
            pcbs[running_index].quantum_used >= config->quantum) {

            int expired_index = running_index;
            running_index = -1;

            ProcessState old_state = pcbs[expired_index].state;
            pcbs[expired_index].state = STATE_READY;
            pcbs[expired_index].ready_order = next_ready_order++;
            pcbs[expired_index].quantum_used = 0;

            ready_queue[ready_count++] = expired_index;

            log_transition(output, current_time,
                           pcbs[expired_index].spec.pid,
                           old_state, pcbs[expired_index].state,
                           REASON_QUANTUM_EXPIRED);
        }

        /*
         * Admit arrivals, in ascending PID order.
         */
        for (size_t i = 0; i < count; i++) {
            if (pcbs[i].state == STATE_NEW &&
                pcbs[i].spec.arrival_time == current_time) {

                ProcessState old_state = pcbs[i].state;
                pcbs[i].state = STATE_READY;
                pcbs[i].ready_order = next_ready_order++;

                ready_queue[ready_count++] = (int)i;

                log_transition(output, current_time,
                               pcbs[i].spec.pid,
                               old_state, pcbs[i].state,
                               REASON_ARRIVAL);
            }
        }

        /*
         *Complete I/O operations, in ascending PID order.
         */
        for (size_t i = 0; i < count; i++) {
            if (pcbs[i].state == STATE_WAITING &&
                pcbs[i].io_completion_time == current_time) {

                ProcessState old_state = pcbs[i].state;
                pcbs[i].state = STATE_READY;
                pcbs[i].ready_order = next_ready_order++;

                ready_queue[ready_count++] = (int)i;

                log_transition(output, current_time,
                               pcbs[i].spec.pid,
                               old_state, pcbs[i].state,
                               REASON_IO_COMPLETE);
            }
        }

        /*
         *SRTF preemption
         */
        if (config->algorithm == ALG_SRTF &&
            running_index != -1 &&
            ready_count > 0) {

            size_t best_position = 0;

            for (size_t i = 1; i < ready_count; i++) {
                int candidate = ready_queue[i];
                int current_best = ready_queue[best_position];

                if (pcbs[candidate].remaining_cpu <
                    pcbs[current_best].remaining_cpu) {
                    best_position = i;
                }
            }

            int best_index = ready_queue[best_position];

            if (pcbs[best_index].remaining_cpu <
                pcbs[running_index].remaining_cpu) {

                int preempted_index = running_index;
                running_index = -1;

                ProcessState old_state = pcbs[preempted_index].state;
                pcbs[preempted_index].state = STATE_READY;
                pcbs[preempted_index].ready_order = next_ready_order++;

                log_transition(output, current_time,
                               pcbs[preempted_index].spec.pid,
                               old_state, pcbs[preempted_index].state,
                               REASON_SRTF_PREEMPTION);

                ready_queue[ready_count++] = preempted_index;
            }
        }

        /*
         *Dispatch and context-switch handling
         */
        bool tick_recorded = false;

        if (running_index == -1 && dispatch_after_switch) {
            dispatch_after_switch = false;

            if (ready_count > 0) {
                size_t selected_position = 0;

                if (config->algorithm == ALG_SRTF) {
                    for (size_t i = 1; i < ready_count; i++) {
                        int candidate = ready_queue[i];
                        int selected = ready_queue[selected_position];

                        if (pcbs[candidate].remaining_cpu <
                                pcbs[selected].remaining_cpu ||
                            (pcbs[candidate].remaining_cpu ==
                                pcbs[selected].remaining_cpu &&
                             (pcbs[candidate].ready_order <
                                  pcbs[selected].ready_order ||
                              (pcbs[candidate].ready_order ==
                                   pcbs[selected].ready_order &&
                               pcbs[candidate].spec.pid <
                                   pcbs[selected].spec.pid)))) {
                            selected_position = i;
                        }
                    }
                }

                running_index = ready_queue[selected_position];

                for (size_t i = selected_position + 1;
                     i < ready_count; i++) {
                    ready_queue[i - 1] = ready_queue[i];
                }
                ready_count--;

                ProcessState old_state = pcbs[running_index].state;
                pcbs[running_index].state = STATE_RUNNING;
                pcbs[running_index].quantum_used = 0;

                log_transition(output, current_time,
                               pcbs[running_index].spec.pid,
                               old_state, pcbs[running_index].state,
                               REASON_DISPATCH);
            }
        }
        else if (running_index == -1 && switch_in_progress) {
            record_timeline_tick(output, current_time,
                                 ACTIVITY_CONTEXT_SWITCH, 0);
            tick_recorded = true;

            switch_remaining--;

            if (switch_remaining == 0) {
                switch_in_progress = false;
                dispatch_after_switch = true;
            }
        }
        else if (running_index == -1 && ready_count > 0) {

            if (config->context_switch > 0) {
                switch_in_progress = true;
                switch_remaining = config->context_switch;

                record_timeline_tick(output, current_time,
                                     ACTIVITY_CONTEXT_SWITCH, 0);
                tick_recorded = true;

                switch_remaining--;

                if (switch_remaining == 0) {
                    switch_in_progress = false;
                    dispatch_after_switch = true;
                }
            }
            else {
                size_t selected_position = 0;

                if (config->algorithm == ALG_SRTF) {
                    for (size_t i = 1; i < ready_count; i++) {
                        int candidate = ready_queue[i];
                        int selected = ready_queue[selected_position];

                        if (pcbs[candidate].remaining_cpu <
                                pcbs[selected].remaining_cpu ||
                            (pcbs[candidate].remaining_cpu ==
                                pcbs[selected].remaining_cpu &&
                             (pcbs[candidate].ready_order <
                                  pcbs[selected].ready_order ||
                              (pcbs[candidate].ready_order ==
                                   pcbs[selected].ready_order &&
                               pcbs[candidate].spec.pid <
                                   pcbs[selected].spec.pid)))) {
                            selected_position = i;
                        }
                    }
                }

                running_index = ready_queue[selected_position];

                for (size_t i = selected_position + 1;
                     i < ready_count; i++) {
                    ready_queue[i - 1] = ready_queue[i];
                }
                ready_count--;

                ProcessState old_state = pcbs[running_index].state;
                pcbs[running_index].state = STATE_RUNNING;
                pcbs[running_index].quantum_used = 0;

                log_transition(output, current_time,
                               pcbs[running_index].spec.pid,
                               old_state, pcbs[running_index].state,
                               REASON_DISPATCH);
            }
        }

        /*
         *Execute exactly one interval and update CPU-burst accounting
         */
        if (!tick_recorded) {
            if (running_index != -1) {
                int executing_index = running_index;

                record_timeline_tick(output, current_time,
                                     ACTIVITY_CPU,
                                     pcbs[executing_index].spec.pid);

                pcbs[executing_index].remaining_cpu--;
                pcbs[executing_index].quantum_used++;

                if (pcbs[executing_index].remaining_cpu == 0) {
                    complete_cpu_burst(pcbs, executing_index,
                                       current_time + 1, output);

                    if (pcbs[executing_index].state ==
                        STATE_TERMINATED) {
                        terminated_count++;
                    } else {
                        pcbs[executing_index].io_completion_time =
                            current_time + 1 +
                            pcbs[executing_index].spec.io_burst;

                        pcbs[executing_index].remaining_cpu =
                            pcbs[executing_index].spec.cpu_burst;
                    }

                    running_index = -1;
                }
            }
            else {
                record_timeline_tick(output, current_time,
                                     ACTIVITY_IDLE, 0);
            }
        }

        if (terminated_count == count) {
            break;
        }

        current_time++;
    }

    bool success = write_metrics(output, specs, count);

    free(ready_queue);
    free(pcbs);

    return success;
}