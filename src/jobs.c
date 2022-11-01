#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <wait.h>
#include <time.h>
#include <errno.h>
#include <sys/time.h>

#include "debug.h"
#include "globals.h"

/*
 * This is the "jobs" module for Mush.
 * It maintains a table of jobs in various stages of execution, and it
 * provides functions for manipulating jobs.
 * Each job contains a pipeline, which is used to initialize the processes,
 * pipelines, and redirections that make up the job.
 * Each job has a job ID, which is an integer value that is used to identify
 * that job when calling the various job manipulation functions.
 *
 * At any given time, a job will have one of the following status values:
 * "new", "running", "completed", "aborted", "canceled".
 * A newly created job starts out in with status "new".
 * It changes to status "running" when the processes that make up the pipeline
 * for that job have been created.
 * A running job becomes "completed" at such time as all the processes in its
 * pipeline have terminated successfully.
 * A running job becomes "aborted" if the last process in its pipeline terminates
 * with a signal that is not the result of the pipeline having been canceled.
 * A running job becomes "canceled" if the jobs_cancel() function was called
 * to cancel it and in addition the last process in the pipeline subsequently
 * terminated with signal SIGKILL.
 *
 * In general, there will be other state information stored for each job,
 * as required by the implementation of the various functions in this module.
 */

typedef enum {
	NEW, RUNNING, COMPLETED, ABORTED, CANCELED
} JOB_STATUS;

typedef struct job {
	int jobId;
	int pgId;
	JOB_STATUS status;
	PIPELINE* pipeline;
	volatile struct job* next;
	char* capturedOutput;
	int canceled;
} Job;

volatile Job* jobTable = NULL;

/**
 * @brief  Initialize the jobs module.
 * @details  This function is used to initialize the jobs module.
 * It must be called exactly once, before any other functions of this
 * module are called.
 *
 * @return 0 if initialization is successful, otherwise -1.
 */
int jobs_init(void) {
	return 0;
}

/**
 * @brief  Finalize the jobs module.
 * @details  This function is used to finalize the jobs module.
 * It must be called exactly once when job processing is to be terminated,
 * before the program exits.  It should cancel all jobs that have not
 * yet terminated, wait for jobs that have been cancelled to terminate,
 * and then expunge all jobs before returning.
 *
 * @return 0 if finalization is completely successful, otherwise -1.
 */
extern ProgStmt* progStore;
extern KVPair* lookup;
int jobs_fini(void) {
	printf("Finishing jobs\n");
	// Iterate over the job table freeing all jobs
	volatile Job* current = jobTable;
	while (current != NULL){
		free_pipeline(current->pipeline);
		free(current->capturedOutput);
		volatile Job* tmp = current->next;
		free((Job*) current);
		current = tmp;
	}

	if (lookup != NULL){
		KVPair* current = lookup;
		while (current != NULL){
			KVPair* tmp = current->next;
			free(current);
			current = tmp;
		}
	}

	if (progStore != NULL){
		ProgStmt* current = progStore;
		while (current != NULL){
			ProgStmt* tmp = current->next;
			free(current);
			current = tmp;
		}
	}
	return 0;
}

/**
 * @brief  Print the current jobs table.
 * @details  This function is used to print the current contents of the jobs
 * table to a specified output stream.  The output should consist of one line
 * per existing job.  Each line should have the following format:
 *
 *    <jobid>\t<pgid>\t<status>\t<pipeline>
 *
 * where <jobid> is the numeric job ID of the job, <status> is one of the
 * following strings: "new", "running", "completed", "aborted", or "canceled",
 * and <pipeline> is the job's pipeline, as printed by function show_pipeline()
 * in the syntax module.  The \t stand for TAB characters.
 *
 * @param file  The output stream to which the job table is to be printed.
 * @return 0  If the jobs table was successfully printed, -1 otherwise.
 */
int jobs_show(FILE *file) {
	// Iterate over the job table
	volatile Job* current = jobTable;
	while (current != NULL){
		char* status;
		switch (current->status){
			case NEW:
				status = "new";
				break;
			case RUNNING:
				status = "running";
				break;
			case COMPLETED:
				status = "completed";
				break;
			case ABORTED:
				status = "aborted";
				break;
			case CANCELED:
				status = "canceled";
				break;
		}
		fprintf(file, "%d\t%d\t%s\t", current->jobId, current->pgId, status);
		show_pipeline(file, current->pipeline);
		current = current->next;
	}
	return 0;
}

int execCommand(COMMAND *command, int pipeIn, int pipeOut){
	// Conver the command to a list of args
	ARG* arg = command->args;
	int cmdCount = 0;
	for (; arg != NULL; arg = arg->next)
		cmdCount++;

	char** args = malloc(sizeof(char*) * cmdCount);
	arg = command->args;
	for (int c=0; c<cmdCount; c++){
		args[c] = eval_to_string(arg->expr);
		arg = arg->next;
	}

	// If we have a non std input, pipe it in
	if (pipeIn != STDIN_FILENO){
		if (dup2(pipeIn, 0) == -1)
			return -1;
		close(pipeIn);
	}

	// If we have a non std output, pipe it out
	if (pipeOut != STDOUT_FILENO){
		if (dup2(pipeOut, 1) == -1)
			return -1;
		close(pipeOut);
	}

	execvp(args[0], args);
	abort();
	return -1;
}

int runLeader(PIPELINE* pline){
	// Set the process group id
	pid_t groupPid = getpid();
	setpgid(groupPid, groupPid);

	// Run through the pipeline commands until the second to last
	COMMAND* currentCommand = pline->commands;
	int pipes[2];
	int prevIn = 0;
	pid_t pid;
	// Open the input file as stdin if supplied
	if (pline->input_file != NULL)
		prevIn = open(pline->input_file, O_RDONLY);
	if (prevIn == -1)
		return -1;

	while (currentCommand->next != NULL){
		if (pipe(pipes) == -1)
			return -1;
		pid = fork();
		if (pid == -1)
			return -1;
		if (pid == 0){
			if (close(pipes[0]) == -1)
				return -1;
			setpgid(0, groupPid);
			execCommand(currentCommand, prevIn, pipes[1]);
			return -1;
		} else {
			if (close(pipes[1]) == -1 || close(prevIn) == -1)
				return -1;
			prevIn = pipes[0];
		}
		currentCommand = currentCommand->next;
	}

	int ultOut = STDOUT_FILENO;
	if (pline->output_file != NULL)
		ultOut = open(pline->output_file, O_WRONLY);
	pid = fork();
	if (pid == 0){
		setpgid(0, groupPid);
		execCommand(currentCommand, prevIn, ultOut);
		return -1;
	} else {
		close(ultOut);
	}

	while (wait(NULL) > 0);
	exit(0);
}

void sigabrt(int sig, siginfo_t *info, void *context){
	// Iterate over the jobs to find a matching job
	volatile Job* current = jobTable;
	for (; current != NULL; current = current->next)
		if (current->pgId == info->si_pid)
			break;

	if (current == NULL)
		return;

	if (current->status == RUNNING)
		current->status = ABORTED;
}

void sigchld(int sig, siginfo_t *info, void *context){
	// Iterate over the jobs to find a matching job
	volatile Job* current = jobTable;
	for (; current != NULL; current = current->next)
		if (current->pgId == info->si_pid)
			break;

	if (current == NULL)
		return;

	if (current->status == RUNNING)
		current->status = COMPLETED;
}

/**
 * @brief  Create a new job to run a pipeline.
 * @details  This function creates a new job and starts it running a specified
 * pipeline.  The pipeline will consist of a "leader" process, which is the direct
 * child of the process that calls this function, plus one child of the leader
 * process to run each command in the pipeline.  All processes in the pipeline
 * should have a process group ID that is equal to the process ID of the leader.
 * The leader process should wait for all of its children to terminate before
 * terminating itself.  The leader should return the exit status of the process
 * running the last command in the pipeline as its own exit status, if that
 * process terminated normally.  If the last process terminated with a signal,
 * then the leader should terminate via SIGABRT.
 *
 * If the "capture_output" flag is set for the pipeline, then the standard output
 * of the last process in the pipeline should be redirected to be the same as
 * the standard output of the pipeline leader, and this output should go via a
 * pipe to the main Mush process, where it should be read and saved in the data
 * store as the value of a variable, as described in the assignment handout.
 * If "capture_output" is not set for the pipeline, but "output_file" is non-NULL,
 * then the standard output of the last process in the pipeline should be redirected
 * to the specified output file.   If "input_file" is set for the pipeline, then
 * the standard input of the process running the first command in the pipeline should
 * be redirected from the specified input file.
 *
 * @param pline  The pipeline to be run.  The jobs module expects this object
 * to be valid for as long as it requires, and it expects to be able to free this
 * object when it is finished with it.  This means that the caller should not pass
 * a pipeline object that is shared with any other data structure, but rather should
 * make a copy to be passed to this function.
 *
 * @return  -1 if the pipeline could not be initialized properly, otherwise the
 * value returned is the job ID assigned to the pipeline.
 */
int jobs_run(PIPELINE *pline) {
	if (pline == NULL || pline->commands == NULL)
		return -1;

	// Choose a job id by finding the last job and increasing jobId by 1
	volatile Job* current = jobTable;
	int lastJID = -2;
	for (; current != NULL && current->next != NULL; current = current->next)
		lastJID = current->jobId;

	int jobId = lastJID+2;

	struct sigaction sigact;
	sigact.sa_sigaction = sigabrt;
	sigact.sa_flags = SA_SIGINFO;
	sigaction(SIGABRT, &sigact, NULL);

	sigact.sa_sigaction = sigchld;
	sigaction(SIGCHLD, &sigact, NULL);

	int pipes[2];
	if (pline->capture_output != 0){
		if (pipe(pipes) == -1)
			return -1;
		fcntl(pipes[0], F_SETFL, O_NONBLOCK);
		fcntl(pipes[0], F_SETFL, O_ASYNC);
		fcntl(pipes[0], F_SETOWN, getpid());
	}

	pid_t pid = fork();
	// If this is the parent, update the job table
	if (pid != 0){
		volatile Job* job = malloc(sizeof(Job));
		job->jobId = jobId;
		job->pgId = pid;
		job->pipeline = pline;
		job->status = NEW;
		job->status = RUNNING;
		job->capturedOutput = NULL;
		job->canceled = 0;
		if (current == NULL)
			jobTable = job;
		else
			current->next = job;

		if (pline->capture_output != 0){
			close(pipes[1]);
			job->capturedOutput = malloc(sizeof(char) * 4096);
			read(pipes[0], job->capturedOutput, sizeof(job->capturedOutput));
		}
		return jobId;
	} else {
		if (pline->capture_output != 0){
			dup2(pipes[1], STDOUT_FILENO);
			close(pipes[0]);
			close(pipes[1]);
		}
		int success = runLeader(pline);
		// If we got a failure, reap all processes in the group and abort
		if (success < 0){
			killpg(getpid(), SIGKILL);
			abort();
		}
	}

	return jobId;
}

volatile Job* findJobById(int jobId){
	// Find the pgid by iterating over the jobs
	volatile Job* current = jobTable;
	for (; current != NULL; current = current->next)
		if (current->jobId == jobId)
			break;
	return current;
}

/**
 * @brief  Wait for a job to terminate.
 * @details  This function is used to wait for the job with a specified job ID
 * to terminate.  A job has terminated when it has entered the COMPLETED, ABORTED,
 * or CANCELED state.
 *
 * @param  jobid  The job ID of the job to wait for.
 * @return  the exit status of the job leader, as returned by waitpid(),
 * or -1 if any error occurs that makes it impossible to wait for the specified job.
 */
int jobs_wait(int jobid) {
	volatile Job* current = findJobById(jobid);
	if (current == NULL)
		return -1;

	int status;
	waitpid(current->pgId, &status, 0);
	return status;
}

/**
 * @brief  Poll to find out if a job has terminated.
 * @details  This function is used to poll whether the job with the specified ID
 * has terminated.  This is similar to jobs_wait(), except that this function returns
 * immediately without waiting if the job has not yet terminated.
 *
 * @param  jobid  The job ID of the job to wait for.
 * @return  the exit status of the job leader, as returned by waitpid(), if the job
 * has terminated, or -1 if the job has not yet terminated or if any other error occurs.
 */
int jobs_poll(int jobid) {
	// Iterate over the jobs to find a match
	volatile Job* current = findJobById(jobid);

	if (current == NULL)
		return -1;

	if (current->status == ABORTED || current->status == COMPLETED || current->status == CANCELED)
		return 0;
	else
		return -1;
}

/**
 * @brief  Expunge a terminated job from the jobs table.
 * @details  This function is used to expunge (remove) a job that has terminated from
 * the jobs table, so that space in the table can be used to start some new job.
 * In order to be expunged, a job must have terminated; if an attempt is made to expunge
 * a job that has not yet terminated, it is an error.  Any resources (exit status,
 * open pipes, captured output, etc.) that were being used by the job are finalized
 * and/or freed and will no longer be available.
 *
 * @param  jobid  The job ID of the job to expunge.
 * @return  0 if the job was successfully expunged, -1 if the job could not be expunged.
 */
int jobs_expunge(int jobid) {
	volatile Job* current = jobTable;
	volatile Job* prev = NULL;
	while (current != NULL){
		if (current->jobId == jobid)
			break;
		prev = current;
		current = current->next;
	}

	if (current == NULL)
		return -1;
	if (current->status == NEW || current->status == RUNNING)
		return -1;

	// If there is no prev this is the first job
	if (prev == NULL)
		jobTable = current->next;
	// Otherwise link next to prev
	else
		prev->next = current->next;

	return 0;
}


void sigkill(int sig, siginfo_t *info, void *context){
	// Iterate over the jobs to find a matching job
	volatile Job* current = jobTable;
	for (; current != NULL; current = current->next)
		if (current->pgId == info->si_pid)
			break;

	if (current == NULL)
		return;

	if (current->status == RUNNING)
		current->status = CANCELED;
}


/**
 * @brief  Attempt to cancel a job.
 * @details  This function is used to attempt to cancel a running job.
 * In order to be canceled, the job must not yet have terminated and there
 * must not have been any previous attempt to cancel the job.
 * Cancellation is attempted by sending SIGKILL to the process group associated
 * with the job.  Cancellation becomes successful, and the job actually enters the canceled
 * state, at such subsequent time as the job leader terminates as a result of SIGKILL.
 * If after attempting cancellation, the job leader terminates other than as a result
 * of SIGKILL, then cancellation is not successful and the state of the job is either
 * COMPLETED or ABORTED, depending on how the job leader terminated.
 *
 * @param  jobid  The job ID of the job to cancel.
 * @return  0 if cancellation was successfully initiated, -1 if the job was already
 * terminated, a previous attempt had been made to cancel the job, or any other
 * error occurred.
 */
int jobs_cancel(int jobid) {
	volatile Job* current = findJobById(jobid);
	if (current == NULL)
		return -1;
	if ((current->status != NEW && current->status != RUNNING) || current->canceled != 0)
		return -1;

	struct sigaction sigact;
	sigact.sa_sigaction = sigkill;
	sigact.sa_flags = SA_SIGINFO;
	sigaction(SIGKILL, &sigact, NULL);

	killpg(current->pgId, SIGKILL);
	current->canceled = 1;
	return 0;
}

/**
 * @brief  Get the captured output of a job.
 * @details  This function is used to retrieve output that was captured from a job
 * that has terminated, but that has not yet been expunged.  Output is captured for a job
 * when the "capture_output" flag is set for its pipeline.
 *
 * @param  jobid  The job ID of the job for which captured output is to be retrieved.
 * @return  The captured output, if the job has terminated and there is captured
 * output available, otherwise NULL.
 */
char *jobs_get_output(int jobid) {
	volatile Job* current = findJobById(jobid);
	if (current == NULL)
		return NULL;
	if (current->status == NEW || current->status == RUNNING)
		return NULL;
	return current->capturedOutput;
}

/**
 * @brief  Pause waiting for a signal indicating a potential job status change.
 * @details  When this function is called it blocks until some signal has been
 * received, at which point the function returns.  It is used to wait for a
 * potential job status change without consuming excessive amounts of CPU time.
 *
 * @return -1 if any error occurred, 0 otherwise.
 */
int jobs_pause(void) {
	sigset_t mask, oldMask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGKILL);
	sigaddset(&mask, SIGCHLD);
	sigaddset(&mask, SIGABRT);

	sigprocmask(SIG_BLOCK, &mask, &oldMask);
//	while (!usr_interrupt)
//		sigsuspend(&oldMask);
//	sigprocmask(SIG_UNBLOCK, &mask, NULL);
	return 0;
}
