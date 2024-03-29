/*********************************************************************
 * Name: smallsh.c
 * Author: Matthew Tinnel
 * Date: 02/05/2023
 * Description:
 *      A small shell program with built-in commands: exit, and cd.
 *      Also supports non-built-in commands, input/output redirection,
 *      comments, background processes, and variable expansion.
 *********************************************************************/

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stddef.h>
#include <fcntl.h>
#include <ctype.h>

#define MAX_LINE 512 /* The maximum length command */

// Struct to hold command line information
typedef struct {
    char *command_array[512];
    int line_count;
    int is_background;
    int is_input_redirection;
    int is_output_redirection;
    char in_file_name[256];
    char out_file_name[256];
} command;

// Global variables will store the exit status of the last foreground process
// and the PID of the last background process
int dollar_question = 0; // The $? parameter shall default to 0 (“0”).
char *dollar_exclamation = ""; // The $! parameter shall default to an empty string (““) if no background process ID is available.

// Initialize SIGINT_action struct to be empty
struct sigaction SIGINT_action = {0}, ignore_action = {0}, old_sigstp_action, old_sigint_action;
static sigjmp_buf env;
static volatile sig_atomic_t jump_bool = false; // make sure this can be accessed by signal handler

/* signal handler for SIGINT, taken from Exploration: Signal Handling API */
void handle_SIGINT_JUMP(){
    if (jump_bool == false) return;
    siglongjmp(env, 1);
}

// Function prototypes
static int GetCommands(command *cmd, char *line, size_t len);
static int ExpandVariables(command *cmd);
static int ParseCommands(command *cmd);
static int ExecuteCommands(command *cmd);
static int ExitShell(command *cmd, pid_t pid);
static int ChangeDirectory(command *cmd);
static int ManageBackgroundProcesses();
int KillChildrenProcesses(pid_t parent_pid, int signal);
char *str_gsub(char *restrict *restrict haystack, char const *restrict needle, char const *restrict sub);

/*******************************************************************************
 * Main function
 ********************************************************************************/
int main(void) {

    // Initialize the command struct
    command cmd;

    // Declare line pointer outside of loop to avoid memory leak
    char *line = NULL;
    size_t len = 0;
    // Enter the main loop, only exit if the user types "exit".
    for (;;) {
        // Clean up
        getcmd:
        cmd.is_background = 0;
        memset(cmd.command_array, 0, sizeof (cmd.command_array));
        memset(cmd.in_file_name, 0, sizeof (cmd.in_file_name));
        memset(cmd.out_file_name, 0, sizeof (cmd.out_file_name));

        cmd.is_input_redirection = 0; cmd.is_output_redirection = 0;
        cmd.line_count = 0;

        GetCommands(&cmd, line, len);
        if (feof(stdin)) goto exit;
        if (cmd.command_array[0] == NULL){ // no command word
            // Go back to get command
            goto getcmd;
        }
        ParseCommands(&cmd);
        ExecuteCommands(&cmd);
    } exit:
    exit(dollar_question);
}

/*****************************************************************
 * char *str_gsub
 * Replace all occurrences of a substring in a string.
 * @param char* haystack
 * @param char* needle
 * @param char* sub
 * @return char* str
 ******************************************************************/
char *str_gsub(char *restrict *restrict haystack, char const *restrict needle, char const *restrict sub){

    char *str = *haystack;
    size_t haystack_len = strlen(str);
    size_t const needle_len = strlen(needle),
            sub_len = strlen(sub);

    for (; (str = strstr(str, needle));) {
        ptrdiff_t off = str - *haystack;

        if (sub_len > needle_len) {
            str = realloc(*haystack, sizeof **haystack * (haystack_len + sub_len - needle_len + 1));
            if (!str) goto exit;
            *haystack = str;
            str = *haystack + off;
        }

        memmove(str + sub_len, str + needle_len, haystack_len + 1 - off - needle_len);
        memcpy(str, sub, sub_len);
        haystack_len += sub_len - needle_len;
        str += sub_len;
    }
    str = *haystack;
    if (sub_len < needle_len){
        str = realloc(*haystack, sizeof **haystack * (haystack_len + 1));
        if (!str) goto exit;
        *haystack = str;
    }
    exit:
    return str;
}

/*********************************************************************
 * GetCommands
 * Print prompt message, get user input, parse it into tokens,
 * and expand variables.
 * @param command* cmd
 * @param char* line
 * @param size_t len
 * @return: 0 if successful, -1 if error
 *********************************************************************/
static int GetCommands(command *cmd, char *line, size_t len) {
    ignore_action.sa_handler = SIG_IGN;
    // Fill out the SIGINT_action struct
    // Register handle_SIGINT as the signal handler
    SIGINT_action.sa_handler = handle_SIGINT_JUMP;
    // Block all catchable signals while handle_SIGINT is running
    sigfillset(&SIGINT_action.sa_mask);

    SIGINT_action.sa_flags = SA_RESTART;

    // Install our signal handlers
    sigaction(SIGINT, &SIGINT_action, NULL);
    sigaction(SIGTSTP, &ignore_action, NULL); // ignore SIGTSTP

    // Set up the sigsetjmp
    jump_point:
    if (sigsetjmp(env, 1) != 0) {
        // If we get here, we jumped back from a SIGINT
        // Clear the input buffer
        putchar('\n');
        fflush(stdin);
        fflush(stdout);
    }
    jump_bool = true;

    /* Managing background processes
    *  Before printing a prompt message, smallsh shall check for any
    *  un-waited-for background processes in the same process group ID as
    *  smallsh, and prints informative message to stderr for each*/
    ManageBackgroundProcesses();

    const char *ps1 = getenv("PS1");
    if (ps1 == NULL) ps1 = "";

    // Print prompt message
    if (fprintf(stderr, "%s", ps1) < 0) goto exit;
    fflush(stdout);

    // Get user input
    ssize_t line_length = getline(&line, &len, stdin); /* Reallocates line */
    if (feof(stdin)){
        fprintf(stderr, "\nexit\n");
        exit(dollar_question);
    }

    if (line_length == 1) goto exit; // no command word
    if (line_length > MAX_LINE) {
        perror("Input line too long");
        exit(1);
    }
    if (line_length == -1) {
        clearerr(stdin); // reset stdin status due to interrupt
        goto jump_point;
    }
        // Process user input
    else {
        char *token;
        int i = 0;
        const char *sep = getenv("IFS");
        if (sep == NULL) sep = " \t\n";
        // Make a copy of each word, store them in cmd.command_array for individual reallocation.
        token = strtok(line, sep);
        char *token_copy = NULL;
        // Store each token in cmd.command_array until # is reached.
        while (token != NULL && strcmp(token, "#") != 0) {
            token_copy = strdup(token);
            cmd->command_array[i] = token_copy;
            token = strtok(NULL, sep);
            i++;
            cmd->line_count++;
        }

        // Expand variables
        ExpandVariables(cmd);

    } exit:
    return errno ? -1 : 0;
}

/*******************************************************************************
 * ExpandVariables
 * Expand variables in command array
 * @param command* cmd
 * @return 0 if successful, -1 if error
 ********************************************************************************/
static int ExpandVariables(command *cmd) {
    // Check each token for variable expansion
    int i = 0;
    while (cmd->command_array[i] != NULL && strcmp(cmd->command_array[i], "#") != 0) {
        // get 2 first chars of token to compare for expansion
        char *first_chars = malloc(3);
        memset(first_chars, 0, 3);
        first_chars[0] = cmd->command_array[i][0];
        first_chars[1] = cmd->command_array[i][1];

        // Expand ~ to home directory
        if (strcmp(first_chars, "~/") == 0) {
            char *home = getenv("HOME");
            str_gsub(&cmd->command_array[i], "~", home);
        }

        // Expand $$ to PID of smallsh
        pid_t pid = getpid();
        char *str_pid = malloc(21);
        if (sprintf(str_pid, "%jd", (intmax_t) pid) < 0) goto exit;
        str_gsub(&cmd->command_array[i], "$$", str_pid);
        free(str_pid);

        // Expand $? to exit status of last foreground command
        char *dollar_question_str = malloc(11);
        if (sprintf(dollar_question_str, "%d", dollar_question) < 0) goto exit;
        str_gsub(&cmd->command_array[i], "$?", dollar_question_str);
        free(dollar_question_str);

        //Expand $! to PID of most recent background process
        str_gsub(&cmd->command_array[i], "$!", dollar_exclamation);

        i++;
    }
    exit:
    return errno ? -1 : 0;
}

/*******************************************************************************
 * ParseCommands
 * Parse the commands in cmd.command_array and handle redirection and background processes
 * @param command* cmd
 * @return 0 if successful, -1 if error
 ********************************************************************************/
static int ParseCommands(command *cmd) {

    //if (cmd.command_array[0] == NULL) goto exit;
    if (cmd->command_array[0] == NULL) return 0;
    ssize_t i = cmd->line_count; // i is the index of the last valid token
    // Parse background indicator
    if (cmd->command_array[i - 1] != NULL && strcmp(cmd->command_array[i - 1], "&") == 0) {
        cmd->is_background = 1;
        free(cmd->command_array[i - 1]);
        cmd->command_array[i - 1] = NULL;
        i--;
        cmd->line_count--;
    }
    // Parse redirection
    if (cmd->command_array[i - 1] != NULL && cmd->command_array[i - 2] != NULL) {
        // Need to loop through these options twice to make sure either order works
        for (int j = 0; j < 2; j++) {
            // Check for input redirection
            if (cmd->is_input_redirection == 0) { // Only set infile once
                if (strcmp(cmd->command_array[i - 2], "<") == 0) {
                    strcpy(cmd->in_file_name, cmd->command_array[i - 1]);
                    free(cmd->command_array[i - 2]);
                    cmd->command_array[i - 2] = NULL;
                    cmd->command_array[i - 1] = NULL;
                    i -= 2;
                    cmd->line_count -= 2;
                    cmd->is_input_redirection = 1;
                }
            }
            // Check for output redirection
            if (cmd->is_output_redirection == 0) { // Only set outfile once
                if (strcmp(cmd->command_array[i - 2], ">") == 0) {
                    strcpy(cmd->out_file_name, cmd->command_array[i - 1]);
                    free(cmd->command_array[i - 2]);
                    cmd->command_array[i - 2] = NULL;
                    cmd->command_array[i - 1] = NULL;
                    i -= 2;
                    cmd->line_count -= 2;
                    cmd->is_output_redirection = 1;
                }
            }
        }
    }
    return errno ? -1 : 0;
}

/*********************************************************************
 * ExecuteCommands
 * Execute the commands stored in cmd.command_array.
 * Includes built-in commands: exit and cd.
 * Supports non-built in commands, and background processes.
 * Handles redirection.
 * @param command* cmd
 * @return: 0 if successful, -1 if error
 *********************************************************************/
static int ExecuteCommands(command *cmd) {

    int err_status = 0;
    int child_status;
    pid_t my_pid = getpid();
    waitpid(-my_pid, &child_status, WNOHANG | WUNTRACED); // Get any child with same group PID as smallsh.

    // Built in commands
    if (cmd->command_array[0] == NULL) goto exit;

    // exit
    if (strcmp(cmd->command_array[0], "exit") == 0) {
        err_status = (ExitShell(cmd, my_pid) < 0) ? -1 : 0;
        goto exit;
    }

    // cd
    if (strcmp(cmd->command_array[0], "cd") == 0) {
        err_status = (ChangeDirectory(cmd) < 0) ? -1 : 0;
        goto exit;
    }

    // Non-Built-in commands
    else{
        // Using exec() with fork()

        // Fork a new process
        pid_t spawn_pid = fork();
        switch(spawn_pid){
            case -1:
                perror("fork()\n");
                err_status = -1;
                goto exit;
            case 0:
                // This runs in the child process.

                // All signals shall be reset to their original actions when smallsh was invoked.
                sigaction(SIGTSTP, &old_sigstp_action, NULL);
                sigaction(SIGINT, &old_sigint_action, NULL);

                // File input
                if (cmd->is_input_redirection == 1){
                    // Open source file
                    int source_file = open(cmd->in_file_name, O_RDONLY);
                    if (source_file == -1){
                        fprintf(stderr, "open() failed on \"%s\"\n", cmd->in_file_name);
                        exit(-1);
                    }
                    // Redirect stdin to source file
                    int result = dup2(source_file , 0);
                    if (result == -1) {
                        perror("source dup2()");
                        exit(-1);
                    }
                    //Close source file
                    fcntl(source_file, F_SETFD, FD_CLOEXEC);
                }

                // File output
                if (cmd->is_output_redirection == 1) {
                    // Open target file
                    int target_file = open(cmd->out_file_name, O_WRONLY | O_CREAT | O_TRUNC, 0777);
                    if (target_file == -1) {
                        perror("target open()");
                        exit(-1);
                    }
                    // Redirect stdout to target file
                    int result = dup2(target_file, 1);
                    if (result == -1) {
                        perror("target dup2()");
                        exit(-1);
                    }
                }

                // Replace the current process image with a new process image
                execvp(cmd->command_array[0], cmd->command_array);

                // exec only returns if there is an error
                perror("execve");
                exit(-1);
            default:
                // In the parent process
                // If not background, blocking wait for child's termination
                if (cmd->is_background == 0){
                    // Wait for child process to terminate
                    spawn_pid = waitpid(spawn_pid, &child_status, WUNTRACED);
                    dollar_question = child_status;
                    if (WIFEXITED(child_status)){
                        dollar_question = WEXITSTATUS(child_status);
                    }
                        // If child process was terminated by a signal, set $? to 128 + signal number
                    else if (WIFSIGNALED(child_status)){
                        dollar_question = 128 + WTERMSIG(child_status);
                    }
                        // If child process was stopped by a signal, send SIGCONT to child process
                        // set $! to child process ID. Print message to stderr.
                    else if (WIFSTOPPED(child_status)) {
                        if (kill(spawn_pid, SIGCONT) < 0) goto exit;
                        if (fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t) spawn_pid) < 0) goto exit;
                        dollar_exclamation = malloc(21);
                        if (sprintf(dollar_exclamation, "%jd", (intmax_t) spawn_pid) < 0) goto exit;
                        goto background_process;
                    }
                    goto exit;
                }
                else {
                    background_process:
                    waitpid(spawn_pid, &child_status, WNOHANG);
                    dollar_exclamation = malloc(21);
                    if (sprintf(dollar_exclamation, "%jd", (intmax_t) spawn_pid) < 0) goto exit;
                    goto exit;
                }
        }
    }
    exit:
    return err_status;
}

/*********************************************************************
 * ExitShell()
 * Handles exit command.
 * If exit argument is an int, exits with that status.
 * If exit argument is not an int, prints error message to stderr.
 * If exit argument is not given, exits with status 0.
 * Kills all child processes before exiting.
 * @param: command *cmd - command struct
 * @param: pid_t my_pid - PID of smallsh
 * @return: -1 if error, otherwise exits shell.
 *********************************************************************/
static int ExitShell(command *cmd, pid_t my_pid){
    if (cmd->command_array[2] != NULL) { // too many arguments
        fprintf(stderr, "exit: too many arguments\n");
        goto exit;
    }
    if (cmd->command_array[1] != NULL) { // exit argument
        // Check if exit argument is an int
        for (size_t i = 0; i < strlen(cmd->command_array[1]); i++){
            if (isdigit(cmd->command_array[1][i]) == 0){
                fprintf(stderr, "exit: argument not an int\n");
                goto exit;
            }
        }
        // Convert exit argument to int
        long exit_status = strtol(cmd->command_array[1], NULL, 10);
        fprintf(stderr, "\nexit\n");
        KillChildrenProcesses(my_pid, SIGINT);
        int err_status = (int) exit_status;
        exit(err_status);
    }
    else { // no exit argument, exit with status of last foreground command
        fprintf(stderr, "\nexit\n");
        KillChildrenProcesses(my_pid, SIGINT);
        exit(dollar_question);
    }
    exit:
    return -1;
}

/*********************************************************************
 * ChangeDirectory()
 * Handles cd command.
 * If no argument, changes directory to home directory.
 * If argument, changes directory to argument.
 * If too many arguments, prints error message to stderr.
 * @param cmd: command struct
 * @return: 0 if successful, -1 if error
 *********************************************************************/
static int ChangeDirectory(command *cmd){
    int err_status = 0;
    if (cmd->command_array[2] != NULL) { // too many arguments
        fprintf(stderr, "smallsh: cd: too many arguments\n");
        err_status = -1;
        goto exit;
    }
    else if (cmd->command_array[1] == NULL) { // no argument, go to home directory
        if (chdir(getenv("HOME")) < 0) {
            err_status = -1;
            goto exit;
        }
    }
    else { // argument, go to directory
        if (chdir(cmd->command_array[1]) < 0) {
            err_status = -1;
            goto exit;
        }
    }
    exit:
    return err_status;
}

/*********************************************************************
 * ManageBackgroundProcesses()
 * Waits for any child processes to terminate or stop.
 * Prints message to stderr if child process was stopped, signaled, or exited.
 * @return: 0 if successful, -1 if error
 *********************************************************************/
static int ManageBackgroundProcesses(){
    int child_status; pid_t my_pid = getpid();
    pid_t child_pid = waitpid(-my_pid, &child_status, WNOHANG | WUNTRACED); // Get any child with same group PID as smallsh.
    while (child_pid > 0) {
        if (WIFSTOPPED(child_status)) {
            if (kill(child_pid, SIGCONT) < 0) goto exit;
            if (fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t) child_pid)) goto exit;
        }
        else if (WIFSIGNALED(child_status)) {
            if (fprintf(stderr, "Child process %jd done. Signaled %d.\n", (intmax_t) child_pid,
                        WTERMSIG(child_status)) < 0)
                goto exit;
        }
        else if (WIFEXITED(child_status)) {
            if (fprintf(stderr, "Child process %jd done. Exit status %d.\n", (intmax_t) child_pid,
                        WEXITSTATUS(child_status)) < 0) goto exit;
        }
        // Get next child
        child_pid = waitpid(-my_pid, &child_status, WNOHANG | WUNTRACED);
    }
    exit:
    return errno ? -1 : 0;
}

/*********************************************************************
 * KillChildrenProcesses()
 * Kills all child processes of parent_pid using Signal.
 * Will not produce an error if there are no child processes.
 * @param pid_t parent_pid
 * @param int Signal
 * @return: 0 if successful, -1 if error
 *********************************************************************/
int KillChildrenProcesses(pid_t parent_pid, int signal){
    int child_status;
    pid_t child_pid = waitpid(-parent_pid, &child_status, WNOHANG); // Get any child with same group PID as smallsh.
    while (child_pid > 0) {
        if (kill(child_pid, signal) < 0) goto exit;
        // Get next child
        child_pid = waitpid(-parent_pid, &child_status, WNOHANG);
    }
    exit:
    return errno ? -1 : 0;
}
