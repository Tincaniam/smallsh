#define _GNU_SOURCE
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

#define MAX_LINE 512 /* The maximum length command */

int dollar_question = 0;
char *dollar_exclamation = "";
int err_status = 0;
int set_infile = 0; int set_outfile = 0;
ssize_t line_count = 0;
int is_background = 0;

static sigjmp_buf env;
static volatile sig_atomic_t jump_bool = false; // make sure this can be accessed by signal handler

/* signal handler for SIGINT, taken from Exploration: Signal Handling API */
void handle_SIGINT_JUMP(int signo){
    if (jump_bool == false) return;
    siglongjmp(env, 1);
}

/*****************************************************************
 * char *str_gsub
 * Taken from String search and replace tutorial by Ryan Gambord
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
 * Print prompt message, get user input, parse it into tokens, and expand variables.
 * @param token_array: array of tokens
 * @return: 0 if successful, -1 if error
 *********************************************************************/
static int GetCommands(char **token_array) {
    // Initialize SIGINT_action struct to be empty
    struct sigaction SIGINT_action = {0}, ignore_action = {0}; 
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
    const char *ps1 = getenv("PS1");
    if (ps1 == NULL) ps1 = "";

    int child_status;
    pid_t my_pid = getpid();
    pid_t child_pid = waitpid(-my_pid, &child_status, 0); // Get any child with same group PID as smallsh.

    while (child_pid > 0) {
        if (WIFEXITED(child_status)) {
            if (fprintf(stderr, "Child process %jd done. Exit status %d\n", (intmax_t) child_pid,
                        WEXITSTATUS(child_status)) < 0) goto exit;
        } else if (WIFSIGNALED(child_status)) {
            if (fprintf(stderr, "Child process %jd done. Terminated by signal %d\n", (intmax_t) child_pid,
                    WTERMSIG(child_status)) < 0) goto exit;
        } else if (WIFSTOPPED(child_status)) {
            if (kill(child_pid, SIGCONT) < 0) goto exit;
            if (fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t) child_pid)) goto exit;
        }
        child_pid = waitpid(-my_pid, &child_status, 0);
    }
    // Print prompt message
    if (printf("%s", ps1) < 0) goto exit;
    fflush(stdout);

    // Get user input
    char *line = NULL;
    size_t n = 0;
    ssize_t line_length = getline(&line, &n, stdin); /* Reallocates line */
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
        // Make a copy of each word, store them in token_array for individual reallocation.
        token = strtok(line, sep);
        char *token_copy = NULL;
        while (token != NULL && strcmp(token, "#") != 0) {
            token_copy = strdup(token);
            token_array[i] = token_copy;
            token = strtok(NULL, sep);
            i++;
            line_count++;
        }

        // Check each token for variable expansion
        int k = 0;
        while (token_array[k] != NULL && strcmp(token_array[k], "#") != 0) {
            // For every token, go through the expansion process 2 chars at a time
            int j = 0;
            while (k < i && j < strlen(token_array[k]) - 1) {
                get_new_chars:; // Label for goto
                if (k >= i) goto exit;
                char *two_chars = malloc(3);
                memset(two_chars, 0, 3);
                two_chars[0] = token_array[k][j]; two_chars[1] = token_array[k][j + 1];
                // Expand ~ to home directory
                if (strcmp(two_chars, "~/") == 0) {
                    char *home = getenv("HOME");
                    str_gsub(&token_array[k], "~", home);
                    j = 0;
                    k++;
                    goto get_new_chars;
                }
                // Expand $$ to PID of smallsh
                else if (strcmp(two_chars, "$$") == 0) {
                    pid_t pid = getpid();
                    char *str_pid = malloc(21);
                    if (sprintf(str_pid, "%jd", (intmax_t) pid) < 0) goto exit;
                    str_gsub(&token_array[k], "$$", str_pid);
                    j = 0;
                    k++;
                    goto get_new_chars;
                }
                // Expand $? to exit status of last foreground command
                // The $? parameter shall default to 0 (“0”).
                else if (strcmp(two_chars, "$?") == 0) {
                    char *dollar_question_str = malloc(11);
                    if (sprintf(dollar_question_str, "%d", dollar_question) < 0) goto exit;
                    str_gsub(&token_array[k], "$?", dollar_question_str);
                    j = 0;
                    k++;
                    goto get_new_chars;
                }
                //Expand $! to PID of most recent background process
                // The $! parameter shall default to an empty string (““).
                else if (strcmp(two_chars, "$!") == 0) {
                    str_gsub(&token_array[k], "$!", dollar_exclamation);
                    j = 0;
                    k++;
                    goto get_new_chars;
                }
                else {
                    j++;
                    free(two_chars);
                }
            } k++;
        }
    } exit:
    return errno ? -1 : 0;
}

/*******************************************************************************
 * ParseCommands
 * Parse the commands passed in and handle redirection and background processes
 * @param token_array
 * @param in_file_name
 * @param out_file_name
 * @return 0 if successful, -1 if error
 ********************************************************************************/
static int ParseCommands(char **token_array, char **in_file_name, char **out_file_name){
    struct sigaction ignore_action = {0};
    sigaction(SIGTSTP, &ignore_action, NULL); // ignore SIGTSTP
    sigaction(SIGINT, &ignore_action, NULL); // ignore SIGINT

    if (token_array[0] == NULL) goto exit;
    ssize_t i = line_count; // i is the index of the last valid token
    // Parse background indicator
    if (token_array[i - 1] != NULL && strcmp(token_array[i - 1], "&") == 0) {
        is_background = 1;
        free(token_array[i - 1]);
        token_array[i - 1] = NULL;
        i--;
        line_count--;
    }
    // Parse redirection
    if (token_array[i - 1] != NULL && token_array[i - 2] != NULL) {
        // Need to loop through these options twice to make sure either order works
        for (int j = 0; j < 2; j++) {
            // Check for input redirection
            if (set_infile == 0) { // Only set infile once
                if (strcmp(token_array[i - 2], "<") == 0) {
                    *in_file_name = token_array[i - 1];
                    free(token_array[i - 2]);
                    token_array[i - 2] = NULL;
                    token_array[i - 1] = NULL;
                    i -= 2;
                    line_count -= 2;
                    set_infile = 1;
                }
            }
            // Check for output redirection
            if (set_outfile == 0) { // Only set outfile once
                if (strcmp(token_array[i - 2], ">") == 0) {
                    *out_file_name = token_array[i - 1];
                    free(token_array[i - 2]);
                    token_array[i - 2] = NULL;
                    token_array[i - 1] = NULL;
                    i -= 2;
                    line_count -= 2;
                    set_outfile = 1;
                }
            }
        }
    }
    exit:
    return errno ? -1 : 0;
}

/*******************************************************************************
 * ExecuteCommands
 * Execute the commands passed in. This function will fork and exec the commands.
 * @param token_array
 * @param in_file_name
 * @param out_file_name
 * @return 0 if successful, -1 if error
 ********************************************************************************/
static int ExecuteCommands(char **token_array, char **in_file_name, char **out_file_name){
    struct sigaction ignore_action = {0}, old_sigstp_action, old_sigint_action;
    sigaction(SIGTSTP, &ignore_action, &old_sigstp_action); // ignore SIGTSTP
    sigaction(SIGINT, &ignore_action, &old_sigint_action); // ignore SIGINT

    if (token_array[0] == NULL) goto exit;

    int child_status;
    pid_t my_pid = getpid();
    pid_t child_pid = waitpid(-my_pid, &child_status, 0); // Get any child with same group PID as smallsh.

    // Built in commands

    // exit
    if (strcmp(token_array[0], "exit") == 0) {
        if (token_array[2] != NULL) { // too many arguments
            fprintf(stderr, "exit: too many arguments\n");
            err_status = -1;
            goto exit;
        }
        if (token_array[1] != NULL) { // exit argument
            long exit_status = strtol(token_array[1], NULL, 10);
            if (exit_status > -256 && exit_status < 256){ // exit argument is an int
                fprintf(stderr, "\nexit\n");
                if (child_pid > 0) {
                    if (kill(0, SIGINT) < 0) exit(1);
                }
                err_status = (int) exit_status;
                exit(err_status);
                //exit(1);
            }
            else { // exit argument is not an int
                fprintf(stderr, "exit: argument not an int\n");
                err_status = -1;
                goto exit;
            }
        }
        else { // no exit argument, exit with status of last foreground command
            fprintf(stderr, "\nexit\n");
            if (child_pid > 0) {
                if (kill(0, SIGINT) < 0) exit(1);
            }
            exit(dollar_question);
            //exit(1);
        }
    }

    // cd
    if (strcmp(token_array[0], "cd") == 0) {
        if (token_array[2] != NULL) { // too many arguments
            fprintf(stderr, "smallsh: cd: too many arguments\n");
            err_status = -1;
            goto exit;
        }
        else if (token_array[1] == NULL) { // no argument, go to home directory
            if (chdir(getenv("HOME")) < 0) goto exit;
        }
        else { // argument, go to directory
            if (chdir(token_array[1]) < 0) goto exit;
        }
        goto exit;
    }

    // Non-Built-in commands
    else{
        // Using exec() with fork(), taken from Exploration: Process API - Executing a New Program

        // Fork a new process
        pid_t spawn_pid = fork();
        switch(spawn_pid){
            case -1:
                perror("fork()\n");
                err_status = -1;
                goto exit;
            case 0:
                // This runs in the child process.

                // All signals shall be reset to their original dispositions when smallsh was invoked.
                sigaction(SIGTSTP, &old_sigstp_action, NULL);
                sigaction(SIGINT, &old_sigint_action, NULL);

                // Taken from Exploration: Processes and I/O
                // File input
                if (set_infile == 1){
                    // Open source file
                    int source_file = open(*in_file_name, O_RDWR | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
                    if (source_file == -1){
                        fprintf(stderr, "open() failed on \"%s\"\n", *in_file_name);
                        exit(-1);
                    }
                    // Redirect stdin to source file
                    int result = dup2(source_file , 1);
                    if (result == -1) {
                        perror("source dup2()");
                        exit(-1);
                    }
                    // Close source file
                    fcntl(source_file, F_SETFD, FD_CLOEXEC);
                }

                // File output
                if (set_outfile == 1) {
                    // Open target file
                    int target_file = open(*out_file_name, O_WRONLY | O_CREAT | O_TRUNC, 0777);
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
                printf("token_array[0]: %s", token_array[0]);
                execvp(token_array[0], token_array);

                // Check for "/" in first character of command
//                char first_char [1] = {};
//                first_char[0] = token_array[0][0];
////                if (strcmp(first_char, "/") != 0){
////                    // Search for command in PATH
////                    execvp(token_array[0], token_array);
////                }
////                else{
////                    // Search for command in current directory
////                    // Search for command in PATH
////                    execv(token_array[0], token_array);
////
////                }
                // exec only returns if there is an error
                perror("execve");
                exit(-1);
            default:
                // In the parent process
                // If not background, blocking wait for child's termination
                if (is_background == 0){
                    // Wait for child process to terminate
                    spawn_pid = waitpid(spawn_pid, &child_status, WUNTRACED);
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
                        //dollar_exclamation = malloc(21);
                        //if (sprintf(dollar_exclamation, "%jd", (intmax_t) spawn_pid) < 0) goto exit;
                        goto background_process;

                    }
                    else {
                        // Set $? to child process exit status
                        dollar_question = child_status;
                    }
                    goto exit;
                }
                else {
                    background_process:
                    // Add to background process list
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

/*******************************************************************************
 * Main function
 ********************************************************************************/
int main(void) {

    struct sigaction ignore_action = {0};
    // Register the ignore_action as the handler for SIGTSTP, and SIGINT.
    // So both of these signals will be ignored.
    sigaction(SIGTSTP, &ignore_action, NULL);
    sigaction(SIGINT, &ignore_action, NULL);

    // User commands will be put in a big array of char *[] to be iterated through
    char *cmd_array[MAX_LINE] = {NULL};
    char *in_file_name[256] = {0};
    char *out_file_name[256] = {0};

    // Enter the main loop, only exit if the user types "exit".
    for (;;) {
        // Clean up
        getcmd:
//        for (int i = 0; i < MAX_LINE; i++) {
//            cmd_array[i] = NULL;
//            free(cmd_array[i]);
//        }
//        for (int i = 0; i < 256; i++) {
//            in_file_name[i] = NULL;
//            out_file_name[i] = NULL;
//            free(in_file_name[i]);
//            free(out_file_name[i]);
//        }
        is_background = 0;
        memset(cmd_array, 0, sizeof(cmd_array));
        memset(in_file_name, 0, sizeof(in_file_name));
        memset(out_file_name, 0, sizeof(out_file_name));
        set_infile = 0; set_outfile = 0;
        line_count = 0;
        err_status = 0;

        GetCommands(cmd_array);
        if (cmd_array[0] == NULL){ // no command word
            // Go back to get command
            goto getcmd;
        }
        ParseCommands(cmd_array, in_file_name, out_file_name);
        ExecuteCommands(cmd_array, in_file_name, out_file_name);
//        int i = 0;
//        while (cmd_array[i] != NULL) {
//            printf("%s\n", cmd_array[i]);
//            i++;
//        }
//        break;
    }
}
