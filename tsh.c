/* 
 * tsh - A tiny shell program with job control
 * 
 * <Wilson Tran & Joshua Teixeira>
 */
 
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/stat.h>///
#include <fcntl.h>///

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2); //system call #32 Copy file descriptor 

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
        switch (c) {
        case 'h':             /* print help message */
            usage();
	    break;
        case 'v':             /* emit additional diagnostic info */
            verbose = 1;
	    break;
        case 'p':             /* don't print a prompt */
            emit_prompt = 0;  /* handy for automatic testing */
	    break;
	default:
            usage();
	}
    }

    /* Install the signal handlers */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

	/* Read command line */
	if (emit_prompt) {
	    printf("%s", prompt);
	    fflush(stdout);
	}
	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
	    app_error("fgets error");
	if (feof(stdin)) { /* End of file (ctrl-d) */
	    fflush(stdout);
	    exit(0);
	}

	/* Evaluate the command line */
	eval(cmdline);
	fflush(stdout);
	fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/


/* ORIGINAL EVAL FROM ASSIGN 3
void eval(char *cmdline) 
{
    char *argv[MAXARGS];
    //int to record for bg   
    int bg;         
    pid_t pid;      
    sigset_t mask; //sig masking in 8.5.4
    
    // parse the line
    bg = parseline(cmdline, argv);
    //check if valid builtin_cmd
    if(!builtin_cmd(argv)) { 
        
        // blocking first, this must be inside the builtin_cmd if statement
        sigemptyset(&mask); //create empty set of sigs to block (also 8.5)
        sigaddset(&mask, SIGCHLD); //add sigchild to sigs to block
        sigprocmask(SIG_BLOCK, &mask, NULL); //begin blocking

        // child
        if((pid = fork()) == 0) { //fork child, and child runs this
            sigprocmask(SIG_UNBLOCK, &mask, NULL);
            setpgid(0, 0); //change child process group id so that our shell ignores stop sigs
            //check if command is there
            if(execvp(argv[0], argv) < 0) { //if exec fails
                printf("%s: Command not found\n", argv[0]);
                exit(0); //child exit
            }
        } 
        // parent adds job
        else {
            if(!bg){ //foreground
                addjob(jobs, pid, FG, cmdline); //add job to job list
				sigprocmask(SIG_UNBLOCK, &mask, NULL); //unblock
				waitfg(pid); //wait for foreground to finish
            }
            else { //background
                addjob(jobs, pid, BG, cmdline); //add to job list
				sigprocmask(SIG_UNBLOCK, &mask, NULL); //unblock
				printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); //print info on bg process
            }
        }
    }
}
*/
//EVAL FOR ASSIGN 5
void eval(char *cmdline) 
{
    char *argv[MAXARGS];
    //int to record for bg   
    int bg;         
    pid_t pid;      
    sigset_t mask; //sig masking in 8.5.4
    
    // parse the line
    bg = parseline(cmdline, argv);
	//to fix segfault on blank lines error
	if (argv[0] == NULL) {  
      return; // ignore blank lines  
    }
	
    //check if valid builtin_cmd
    if(!builtin_cmd(argv)) { 
        
        // blocking first, this must be inside the builtin_cmd if statement
        sigemptyset(&mask); //create empty set of sigs to block (also 8.5)
        sigaddset(&mask, SIGCHLD); //add sigchild to sigs to block
        sigprocmask(SIG_BLOCK, &mask, NULL); //begin blocking

        int infd = 0;
        int outfd = 0;
        int child_fd[2] = {-1, -1};
        char **arg = argv;
        char **argvT = NULL;

        while (*arg){
            if (!strcmp(*arg, "<")){
                if ((infd = open(*(arg + 1), O_RDONLY)) < 0){
                    perror("Could not open input file (<)");
                }
                *arg = NULL;
            } else if (!strcmp(*arg, ">")){
                if ((outfd = open(*(arg + 1), O_WRONLY | O_CREAT | O_TRUNC, 0666)) < 0){
                    perror("Could not open output file (>)");
                }
                *arg = NULL;
            } else if (!strcmp(*arg, ">>")){
                if ((outfd = open(*(arg + 1), O_WRONLY | O_APPEND | O_CREAT, 0666)) < 0){
                    perror("Could not open output file (>>)");
                }
                *arg = NULL;
            } else if (!strcmp(*arg, "2>")){
                if ((outfd = open(*(arg + 1), O_WRONLY | O_CREAT, 0666 | O_TRUNC)) < 0){
                    perror("Could not open output file (2>)");
                }
                *arg = NULL;
            } else if (!strcmp(*arg, "|")){
                if (pipe(child_fd) < 0){
                    unix_error("Piping error");
                }
                *arg = NULL;
                argvT = arg + 1;
            }
            arg++;
        }

        // child
        if((pid = fork()) == 0) { //fork child, and child runs this
            sigprocmask(SIG_UNBLOCK, &mask, NULL);
            setpgid(0, 0); //change child process group id so that our shell ignores stop sigs
            
            if (infd > 0){
                dup2(infd, 0);
                close(infd);
            }
            if (outfd > 0){// hmmm
                dup2(outfd, 1);
                close(outfd);
            }
			
			
			if (child_fd[1] > 0){
                
                dup2(child_fd[1], 1);
                close(child_fd[0]);
                close(child_fd[1]);
            } else if (outfd > 0){
                dup2(outfd, 1);
                close(outfd);
            }
			
			
            //check if command is there
            if(execvp(argv[0], argv) < 0) { //if exec fails
                printf("%s: Command not found\n", argv[0]);
                exit(0); //child exit
            }
        } 
        if (argvT != NULL){
            pid_t pid2 = fork();
            if (pid2 < 0)
                unix_error("fork");
            if (pid2 == 0){
                sigprocmask(SIG_UNBLOCK, &mask, NULL);
                setpgid(pid, pid);
                
                dup2(child_fd[0], 0);
                close(child_fd[0]);
                close(child_fd[1]);
                
                if (outfd > 0){
                    dup2(outfd, 1);
                    close(outfd);
                }
                if (execvp(argvT[0], argvT) < 0){
                    printf("%s: command not found.\n", argv[0]);
                    exit(0);
                }
            }
        }
        if (infd > 0){
            close(infd);
        }
        if (outfd > 0){
            close(outfd);
        }
        if (child_fd[0] > 0){
            close(child_fd[0]);
            close(child_fd[1]);
        }
        // parent adds job
        else {
            if(!bg){ //foreground
                addjob(jobs, pid, FG, cmdline); //add job to job list
				sigprocmask(SIG_UNBLOCK, &mask, NULL); //unblock
				waitfg(pid); //wait for foreground to finish
            }
            else { //background
                addjob(jobs, pid, BG, cmdline); //add to job list
				sigprocmask(SIG_UNBLOCK, &mask, NULL); //unblock
				printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline); //print info on bg process
            }
        }
    }
}


/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
	buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
	buf++;
	delim = strchr(buf, '\'');
    }
    else {
	delim = strchr(buf, ' ');
    }

    while (delim) {
	argv[argc++] = buf;
	*delim = '\0';
	buf = delim + 1;
	while (*buf && (*buf == ' ')) /* ignore spaces */
	       buf++;

	if (*buf == '\'') {
	    buf++;
	    delim = strchr(buf, '\'');
	}
	else {
	    delim = strchr(buf, ' ');
	}
    }
    argv[argc] = NULL;
    
    if (argc == 0)  /* ignore blank line */
	return 1;

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
	argv[--argc] = NULL;
    }
    return bg;
}



int builtin_cmd(char **argv) 
{	
	if(!strcmp(argv[0], "jobs")){ //if "jobs" is typed
		listjobs(jobs); //use builtin listjobs
		return 1;
	}
	else if(!strcmp(argv[0], "bg") || !strcmp(argv[0], "fg")){ //if bg or fg, do bgfg funct
		do_bgfg(argv);
		return 1;
	}
	else if(!strcmp(argv[0], "quit")){ //if quit, quit
		exit(0);
	}			
    return 0; //if not builtin_cmd
}


/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
 
void do_bgfg(char **argv) 
{
	char *firstArgument = argv[0]; //  bg or fg
	char *secondArgument = argv[1]; // argument 
	
	//this must be up here, otherwise next declarations may access mem they shouldnt
	if (secondArgument == NULL) {
		printf("%s command requires PID or %%jobid argument\n", firstArgument);
		return;
	}
	//(continue declarations)
	char firstChar = secondArgument[0];
	char secondChar = secondArgument[1];
	struct job_t* newJob;
	pid_t newPid;
	int newJid;
	
	if (firstChar != '%' && !isdigit(firstChar)){ //if arg isnt an num and doesnt have %, must not be a pid or jid
		printf("%s: argument must be a PID or %%jobid\n", firstArgument);
		return;	
	}
	else{
		if (firstChar == '%'){ //Job ID
			newJid = atoi(&secondChar);
			newJob = getjobjid(jobs, newJid);
			if (newJob == NULL){ //null or 0?
				printf("%s: No such job\n", secondArgument); //if job is null after user used jid
				return;
			}
		}else{//PID
			newPid = atoi(secondArgument);
			newJob = getjobpid(jobs, newPid);
			if (newJob == NULL){ //null or 0?
				printf("(%d): No such process\n", newPid); //if job is null after usuer used pid
				return;
			}
		}
	}
	//if arg is "bg"
	if( firstArgument[0] == 'b' && firstArgument[1] == 'g'){
		(*newJob).state = BG;
		printf("[%d] (%d) %s",(*newJob).jid,(*newJob).pid,(*newJob).cmdline);
		kill(-(*newJob).pid, SIGCONT);
	//if arg is "fg"
	}else if ( firstArgument[0] == 'f' && firstArgument[1] == 'g' ){
		(*newJob).state = FG;
		kill(-(*newJob).pid, SIGCONT);
		waitfg((*newJob).pid);
	}
	return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
	int flag = 1;
	struct job_t* temp;
	temp = getjobpid(jobs, pid); // Retrieving foreground job
	while(flag){
		if (temp->state == FG) { //If it's in the foreground
			sleep(1); 	// Go to sleep
			//printf("test ");
			continue;	// Continues loop
		}
		flag = 0;	//Exits the loop when job is no longer foreground
		break;
	}
    return;
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */

void sigchld_handler(int sig) 
{
    pid_t pid;
    int status;
    int jobid;

    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) { //wait for any child process   
        jobid = pid2jid(pid);

        if (WIFEXITED(status)) {
            deletejob(jobs, pid); //delete job, if this is ran then the child termed normally
        }
        else if (WIFSIGNALED(status)) {
            deletejob(jobs,pid); //delete job, this is ran if the child termed due to a signal
            printf("Job [%d] (%d) terminated by signal %d\n", jobid, (int) pid, WTERMSIG(status));
        }
        
        else if (WIFSTOPPED(status)) {    //if the child is stopped
            getjobpid(jobs, pid)->state = ST; //stop job
            printf("Job [%d] (%d) stopped by signal %d\n", jobid, (int) pid, WSTOPSIG(status));
        }
        
    }
    return;
}


/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
	int temp = fgpid(jobs); 	// Gets pid of foreground job
	if (temp != 0){				// If there's a foreground job
		kill(-temp, SIGINT);	// Send SIGINT signal in FG
	}
	
    return;
}


/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
	int temp = fgpid(jobs); //get pid of foreground job
	if (temp != 0){			//if there IS a forground job
		kill(-temp, SIGTSTP); //kill job based on pid
	}
    return;
}


/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid > max)
	    max = jobs[i].jid;
    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == 0) {
	    jobs[i].pid = pid;
	    jobs[i].state = state;
	    jobs[i].jid = nextjid++;
	    if (nextjid > MAXJOBS)
		nextjid = 1;
	    strcpy(jobs[i].cmdline, cmdline);
  	    if(verbose){
	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;

    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid == pid) {
	    clearjob(&jobs[i]);
	    nextjid = maxjid(jobs)+1;
	    return 1;
	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].state == FG)
	    return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid)
	    return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
	return NULL;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].jid == jid)
	    return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1)
	return 0;
    for (i = 0; i < MAXJOBS; i++)
	if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
	if (jobs[i].pid != 0) {
	    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
	    switch (jobs[i].state) {
		case BG: 
		    printf("Running ");
		    break;
		case FG: 
		    printf("Foreground ");
		    break;
		case ST: 
		    printf("Stopped ");
		    break;
	    default:
		    printf("listjobs: Internal error: job[%d].state=%d ", 
			   i, jobs[i].state);
	    }
	    printf("%s", jobs[i].cmdline);
	}
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
	unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
    //return 0; //placeholder return?
    return;
}

