#include "3140_concur.h"
#include <stdlib.h>
#include <fsl_device_registers.h>
#include "realtime.h"

#define T1ms 0x1D4C0 //One millisecond (based on 120 MHz clock - same as processor)

struct process_state {
			unsigned int *sp;
			unsigned int *sp_original; //Original stack pointer
			unsigned int size; //Size of stack as initialized
			realtime_t *start; //Absolute Start time. Null if not real-time
			realtime_t *deadline; //Absolute Deadline time. Null if not real-time
			struct process_state *nextProcess;
}; 

//queue sorted by start time. No element of the
//queue should be ready to execute (those should
//go in deadline_queue. A value of NULL means that
//there are no real-time processes which are not
//ready to be executed.
struct process_state * start_time_queue = NULL;
//queue sorted by deadline. All elements of the
//queue must be ready to execute.
struct process_state * deadline_queue = NULL;
//queue of non real-time processes for round robin execution
//A value of NULL means there are no non-real time processes
//to be executed.
struct process_state * round_robin_queue = NULL;
//points to the process in deadline_queue that is currently
//executing. A value of NULL means that deadline_queue is empty
//and there are no processes ready to be executed.
struct process_state * current_realtime_process = NULL;


//The queue will be created with process_create in begin_queue
//then switched to current_process during the first call to process_select
unsigned int first_time=1;

int process_deadline_met=0;
int process_deadline_miss=0;
realtime_t current_time={0,0};

/*
*[append process queue] appends [process] to [queue] handling the case
*where [queue] is NULL. [process] may not be NULL.
*
*/

void append(struct process_state * lastElement){
			struct process_state *tmp;
			if (round_robin_queue == NULL) {
				round_robin_queue = lastElement; //You are assigning to a pointer rather than the value
			} else {
				tmp = round_robin_queue;
				while (tmp->nextProcess != NULL) {
					// while there are more elements in the list
					tmp = tmp->nextProcess;
				}
				// now tmp is the last element in the list
				tmp->nextProcess = lastElement;
			}
			lastElement->nextProcess = NULL;
}

//Returns 0 if compareTo earlier than now, 1 if same, 2 if later
int compareTimes(realtime_t * now, realtime_t * compareTo) {
	if (now->sec < compareTo->sec)
		return 2;
	if (now->sec > compareTo->sec)
		return 0;
	if (now->msec < compareTo->msec)
		return 2;
	if (now->msec > compareTo->sec)
		return 0;
	return 1;
}

void addDeadlineQueue(struct process_state * process) {
	//If earlier than the earliest process, put at the beginning
	if (compareTimes(start_time_queue->deadline,process->deadline)<2) {
		process->nextProcess=deadline_queue;
		deadline_queue=process;
	}
	else {
		struct process_state *temp, *earlier;
		earlier=deadline_queue;
		temp=earlier->nextProcess;
		while (1) {
			//Puts process before temp if deadline time is the same or earlier
			if (compareTimes(temp->deadline,process->deadline)<2) { //Is earlier than the temp process
				process->nextProcess=temp;
				earlier->nextProcess=process;
				break;
			}
			if (temp->nextProcess==NULL) { //Has latest deadline time - put at end
				temp->nextProcess = process;
				break;
			}
			//Moves to next process in list
			earlier=temp;
			temp=earlier->nextProcess;
		}
	}
}

void addRealTime(struct process_state * process) {
	//Not yet ready
	if (compareTimes(&current_time, process->start)==0) {
		//If earlier than the earliest process, put at the beginning
		if (compareTimes(start_time_queue->start,process->start)<2) {
			process->nextProcess=start_time_queue;
			start_time_queue=process;
		}
		else {
			struct process_state *temp, *earlier;
			earlier=start_time_queue;
			temp=earlier->nextProcess;
			while (1) {
				//Puts process before temp if start time is the same or earlier
				if (compareTimes(temp->start,process->start)<2) { //Is earlier than the temp process
					process->nextProcess=temp;
					earlier->nextProcess=process;
					break;
				}
				if (temp->nextProcess==NULL) { //Has latest start time - put at end
					temp->nextProcess = process;
					break;
				}
				//Moves to next process in list
				earlier=temp;
				temp=earlier->nextProcess;
			}
		}
	}
	//Already past start time - need to put in deadline queue
	addDeadlineQueue(process);
}
	
/*
*[remove queue] removes the first element from [queue]
*/
struct process_state* remove(struct process_state * queue) {
	//Creating new pointer to process at top of list
	struct process_state * removed_process = queue;
	//Moving next process up, effectively removing top process
	if (queue != NULL) {
		queue = removed_process->nextProcess;
	}
	removed_process->nextProcess = NULL;
	//Returning the pointer to the top process;
	return removed_process;
}

/*
*[process_init n] initializes a new struct process_state
*with at least [n] memory allocated to it. Returns NULL on
*malloc issues and the processState otherwise. Does not
*set the 'start' or 'deadline' attributes to any value.
*/
struct process_state* process_init(void (*f)(void), int n) {
	unsigned int *sp = process_stack_init(*f, n);
	if (sp == NULL) return NULL;
	struct process_state *processState = malloc(sizeof(*processState));
	if (processState == NULL) return NULL;
	processState->sp = sp;
	processState->sp_original = sp;
	processState->size=n;
	return processState;
}

int process_create (void (*f)(void), int n) {
	struct process_state* processState = process_init(f, n);
	if (processState == NULL) return -1;
	else {
		processState->start=NULL; //NULL Indicates a static (not realtime) process
		processState->deadline=NULL;
		append(processState);
		return 0;
	}
};

int process_rt_create(void (*f)(void), int n, realtime_t *start, realtime_t *deadline) {
	struct process_state* processState = process_init(f, n);
	if (processState == NULL) return -1;
	else {
		realtime_t start_time = {current_time.sec+start->sec,current_time.msec+start->msec};
		realtime_t deadline_time = {start_time.sec+deadline->sec,start_time.msec+deadline->msec};
		processState->start=&start_time;
		processState->deadline=&deadline_time;
		addRealTime(processState); //TO DO - adds to appropriate queue
		return 0;
	}
}

void process_start (void) {
	
	NVIC_EnableIRQ(PIT0_IRQn); //Enables interrupts timer 0
	NVIC_EnableIRQ(PIT1_IRQn); //Enables interrupts timer 1
	
	//Establishes appriority priority order
	NVIC_SetPriority(SVCall_IRQn,1);
	NVIC_SetPriority(PIT0_IRQn,2);
	NVIC_SetPriority(PIT1_IRQn,0);
	
	SIM->SCGC6 = SIM_SCGC6_PIT_MASK;
	PIT_MCR = 1; //Enables standard timers
	PIT->CHANNEL[0].LDVAL = 3000000;
	PIT->CHANNEL[1].LDVAL = T1ms;
	
	process_begin();
}

/*
* [move_processes] moves any processes which are ready 
* from the start_time_queue to the deadline_queue.
*/
void move_processes(void) {
	struct process_state *temp, *earlier;
	earlier=deadline_queue;
	temp=earlier->nextProcess;
	while (1) {
		//Iterates through processes, removes if current_time start is after current_time
		if (compareTimes(&current_time,temp->start)>0) { //If the start time is after the current time
				earlier->nextProcess=temp->nextProcess;
				addDeadlineQueue(temp);
		}
		if (temp->nextProcess==NULL)
			break;
		//Moves to next process in list
		earlier=temp;
		temp=earlier->nextProcess;
	}
}

/*
* [update_sp cursp] identifies the process which is currently
* running and sets it's sp to [cursp]
*/
void update_sp(unsigned int *cursp) {
	if (current_realtime_process != NULL)
		current_realtime_process->sp = cursp;
	else if (round_robin_queue != NULL)
		round_robin_queue->sp = cursp;
}

/*
* [free_process terminated] frees up all the memory allocated
* to the process [terminated].
*/
void free_process(struct process_state * terminated_process) {
	//Free the stack of the process
	process_stack_free(terminated_process->sp_original, terminated_process->size);
	//Free the struct holding the process
	free(terminated_process);
}

/*
* [remove_static] removes the first process from the 
* [round_robin_queue] and frees it.
*/
void remove_static(void) {
	struct process_state * terminated = remove(round_robin_queue);
	free_process(terminated);
}

/*
* [remove_realtime] removes the [current_realtime_process]
* from the [deadline_queue] and frees it.
*/
void remove_realtime(void) {
	//Updates deadline count
	if (compareTimes(&current_time,current_realtime_process->deadline) == 0) //Missed deadline
		process_deadline_miss++;
	else
		process_deadline_met++;
	
	if (current_realtime_process == deadline_queue) {
		deadline_queue = NULL;
	} else {
			struct process_state* tmp = deadline_queue;
			while (tmp->nextProcess != current_realtime_process) {
				tmp = tmp->nextProcess;
			}
			tmp->nextProcess = current_realtime_process->nextProcess;
			current_realtime_process->nextProcess = NULL;
	}
	free_process(current_realtime_process);
	current_realtime_process = NULL;
}

/*
* [pop_and_push] pops an element from [round_robin_queue]
* and pushes it back on, effectively moving the first
* process to the end of the queue.
*/
void pop_and_push(void) {
	struct process_state * switched_process = remove(round_robin_queue);
	append(switched_process);
}

/*
* [get_next_process] returns the next process to
* be executed or NULL if there are no processes
* remaining. This stalls as necessary if a process
* is not yet ready. It also adjusts the
* [current_realtime_process] to reflect the newly
* selected process.
*/
struct process_state* get_next_process(void) {
	if (deadline_queue == NULL) {
		current_realtime_process = NULL;
		if (round_robin_queue == NULL) {
			if (start_time_queue == NULL) {
				return NULL;
			} else {
				__enable_irq();
				while (deadline_queue == NULL);
				__disable_irq();
				return get_next_process();
			}
		} else {
			return round_robin_queue;
		}
	} else {
		current_realtime_process = deadline_queue;
		return deadline_queue;
	}
}
	
unsigned int * process_select (unsigned int *cursp) {
	//TO DO - Update real time queues, moving ready processes into the ready queue (in order)
	//Update process sp
	if (!first_time)
		update_sp(cursp);

	//If a process terminates, handle that appropriately
	//Switch to the process at the beginning of the deadline queue (can be the same)
	
	//If there are no process in the deadline queue, run the appropriate static process
	
	if (cursp==NULL) {
		if (first_time) {
			//This is the first call to process_select
			first_time=0;
		} else {
			//A process has just terminated
			//Remove the process from the queue
			if (current_realtime_process == NULL) {
				remove_static();
			} else {
				remove_realtime();
			}
		}
	}
	else {
		//Switching from one running process to another
		
		//pop and push in the case of round robin
		if (current_realtime_process == NULL) {
			pop_and_push();
		}
	}
	struct process_state* processState = get_next_process();
	return processState->sp;
}

void PIT1_IRQHandler(void) {
	PIT->CHANNEL[1].LDVAL = T1ms; //Resets timer with 1 ms
	PIT_TFLG0 = 1; //Clears the timeout flag
	
	if (current_time.msec < 1000)
		current_time.msec++;
	else {
		current_time.msec = 0;
		current_time.sec++;
	}
	
	move_processes();
}
