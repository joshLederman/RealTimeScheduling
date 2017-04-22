#include "3140_concur.h"
#include <stdlib.h>
#include <fsl_device_registers.h>
#include "realtime.h"

#define T1ms 0x1D4C0 //One millisecond (based on 120 MHz clock - same as processor)

struct process_state {
			unsigned int *sp;
			unsigned int *sp_original; //Original stack pointer
			unsigned int size; //Size of stack as initialized
			struct process_state *nextProcess;
}; 

//queue sorted by start time. No element of the
//queue should be ready to execute (those should
//go in deadline_queue
struct process_state * start_time_queue = NULL;
//queue sorted by deadline. All elements of the
//queue must be ready to execute.
struct process_state * deadline_queue = NULL;

struct process_state * current_process = NULL;
//The queue will be created with process_create in begin_queue
//then switched to current_process during the first call to process_select
unsigned int first_time=1;

void append(struct process_state * lastElement){
			struct process_state *tmp;
			//current_process - list of process_state
			if (current_process == NULL) {
				current_process = lastElement;
				lastElement->nextProcess = NULL;
			}
			else {
				tmp = current_process;
				while (tmp->nextProcess != NULL) {
					// while there are more elements in the list
					tmp = tmp->nextProcess;
				}
				// now tmp is the last element in the list
				tmp->nextProcess = lastElement;
				lastElement->nextProcess = NULL;
			}
}

//Removes first process of queue
struct process_state* remove() {
	//Creating new pointer to process at top of list
	struct process_state * removed_process = current_process;
	//Moving next process up, effectively removing top process
	current_process = removed_process->nextProcess;
	//Returning the pointer to the top process;
	return removed_process;
}

int process_create (void (*f)(void), int n) {
	unsigned int *sp = process_stack_init(*f, n);
	if (sp == NULL) return -1;
	struct process_state *processState = malloc(sizeof(*processState));
	processState->sp = sp;
	processState->sp_original = sp;
	processState->size=n;
	append(processState);
	return 0;
};

int process_rt_create(void (*f)(void), int n, realtime_t *start, realtime_t *deadline) {
	
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
	
unsigned int * process_select (unsigned int *cursp) {
	if (cursp==NULL) {
		if (first_time) {
			//This is the first call to process_select
			first_time=0;
			return current_process->sp;
		}
		else {
			//A process has just terminated
			
			//Remove the process from the queue
			struct process_state * terminated_process = remove();
			//Free the stack of the process
			process_stack_free(terminated_process->sp_original, terminated_process->size);
			//Free the struct holding the process
			free(terminated_process);
			//Return the next process
			
			//No more processes - return NULL
			if (current_process == NULL)
				return NULL;
			return current_process->sp;
		}
	}
	else {
		//Switching from one running process to another
		
		//Store the next sp
		current_process->sp=cursp;
		//Remove the top process from the queue and add to end
		struct process_state * switched_process = remove();
		append(switched_process);
		//Returns the new process sp
		return current_process->sp;
	}
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
}