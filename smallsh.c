#define _GNU_SOURCE
#include <err.h>
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
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <fcntl.h>

#define MAX_LINE 512 /* The maximum length command */

int dollar_question = 0;
char *dollar_exclamation = "";
int err_status = 0;
int set_infile = 0; int set_outfile = 0;
ssize_t line_count = 0;
ssize_t free_line_count = 0;
int is_background = 0;

static sigjmp_buf env;
static volatile sig_atomic_t jump_bool = false; // make sure this can be accessed by signal handler

/* signal handler for SIGINT, taken from Exploration: Signal Handling API */
void handle_SIGINT_JUMP(int signo){
    if (jump_bool == false) return;
    siglongjmp(env, 1);
}

/********************************************************************************
 * Taken from String search and replace tutorial by Ryan Gambord
 * @param haystack
 * @param needle
 * @param sub
 * @return string
 */
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

/********************************************************************************
 * Taken from String search and replace tutorial by Ryan Gambord
 * @param char* token_array[]
 * @param char*line
 * @param size_t n,
 * @return const char* ps1
 */
static int GetCommands(char **token_array, char *line, size_t n, const char *ps1) {
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
    // Get user input
    ssize_t line_length = getline(&line, &n, stdin); /* Reallocates line */
    free_line_count = line_length;
    if (line_length == 1) goto exit; // no command word
    if (line_length > MAX_LINE) {
        perror("Input line too long");
        exit(1);
    }
    if (line_length == -1) {
        clearerr(stdin); // reset stdin status due to interrupt
        goto jump_point;

    } else { // process the input
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

        // Expansion
        int k = 0;
        while (token_array[k] != NULL && strcmp(token_array[k], "#") != 0) {
            // get 2 first chars of token to compare for expansion
            char first_chars [2] = {};
            first_chars[0] = token_array[k][0]; first_chars[1] = token_array[k][1];
            if (strcmp(first_chars, "~/") == 0) {
                char *home = getenv("HOME");
                str_gsub(&token_array[k], "~", home);
            }

            // Expand $$ to PID of smallsh
            pid_t pid = getpid();
            char *str_pid = malloc(21);
            if (sprintf(str_pid, "%jd", (intmax_t) pid) < 0) goto exit;
            str_gsub(&token_array[k], "$$", str_pid);
            free(str_pid);

            // Expand $? to exit status of last foreground command
            // The $? parameter shall default to 0 (“0”).
            char *dollar_question_str = malloc(11);
            if (sprintf(dollar_question_str, "%d", dollar_question) < 0) goto exit;
            str_gsub(&token_array[k], "$?", dollar_question_str);
            free(dollar_question_str);

            //Expand $! to PID of most recent background process
            // The $! parameter shall default to an empty string (““) if no background process ID is available.
            str_gsub(&token_array[k], "$!", dollar_exclamation);

            k++;
        }
    } exit:
    return errno ? -1 : 0;
}

/*********************************************************************
 * main
 *********************************************************************/
static int ParseCommands(char **token_array, char **in_file_name, char **out_file_name){
    struct sigaction ignore_action = {0};
    sigaction(SIGTSTP, &ignore_action, NULL); // ignore SIGTSTP
    sigaction(SIGINT, &ignore_action, NULL); // ignore SIGINT

    if (token_array[0] == NULL) goto exit;
    int i = 0;
    // iterate to end of token array
    while (token_array[i] != NULL && strcmp(token_array[i], "#") != 0) {
        i++;
    }
    if (token_array[i - 1] != NULL && strcmp(token_array[i - 1], "&") == 0) {
        // Parse background indicator
        is_background = 1;
        free(token_array[i - 1]);
        token_array[i - 1] = NULL;
        i--;
    }
    if (token_array[i - 1] != NULL && token_array[i - 2] != NULL) {
        // Need to loop through these options twice to make sure either order works
        for (int j = 0; j < 2; j++) {
            if (set_infile == 0) {
                if (strcmp(token_array[i - 2], "<") == 0) {
                    *in_file_name = token_array[i - 1];
                    free(token_array[i - 2]);
                    token_array[i - 2] = NULL;
                    token_array[i - 1] = NULL;
                    i -= 2;
                    set_infile = 1;
                }
            }
            if (set_outfile == 0) {
                if (strcmp(token_array[i - 2], ">") == 0) {
                    *out_file_name = token_array[i - 1];
                    free(token_array[i - 2]);
                    token_array[i - 2] = NULL;
                    token_array[i - 1] = NULL;
                    i -= 2;
                    set_outfile = 1;
                }
            }
        }
    }
    exit:

    return errno ? -1 : 0;
}

/*********************************************************************
 * main
 *********************************************************************/
static int ExecuteCommands(char **token_array, char **in_file_name, char **out_file_name){
    struct sigaction ignore_action = {0}, old_sigstp_action, old_sigint_action;
    sigaction(SIGTSTP, &ignore_action, &old_sigstp_action); // ignore SIGTSTP
    sigaction(SIGINT, &ignore_action, &old_sigint_action); // ignore SIGINT

    if (token_array[0] == NULL) goto exit;

    // Built in commands

    // cd
    if (strcmp(token_array[0], "cd") == 0) {
        if (token_array[2] != NULL) {
            fprintf(stderr, "smallsh: cd: too many arguments\n");
            err_status = 1;
            goto exit;
        }
        else if (token_array[1] == NULL) {
            if (chdir(getenv("HOME")) < 0) goto exit;
        }
        else {
            if (chdir(token_array[1]) < 0) goto exit;
        }

        goto exit;
    }

    // Non-Built-in commands
    else{
        // Using exec() with fork(), taken from Exploration: Process API - Executing a New Program
        int child_status;

        // Get command and arguments
//        char *cmd = token_array[0];
//        char *args[512] = {};
//        int i = 1; int j = 0;
//        while (token_array[i] != NULL && strcmp(token_array[i], "#") != 0) {
//            args[j] = token_array[i];
//            i++; j ++;
//        }

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
                        printf("open() failed on \"%s\"\n", *in_file_name);
                        err_status = 1;
                        goto exit;
                    }

                    // Redirect stdin to source file
                    int result = dup2(source_file , 1);
                    if (result == -1) {
                        perror("source dup2()");
                        err_status = 2;
                        goto exit;
                    }

                    // Close source file
                    fcntl(source_file, F_SETFD, FD_CLOEXEC);
                }

                // File output
                if (set_outfile == 1) {
                    // Open target file
                    int target_file = open(*out_file_name, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (target_file == -1) {
                        perror("target open()");
                        err_status = 1;
                        goto exit;
                    }

                    // Redirect stdout to target file
                    int result = dup2(target_file, 1);
                    if (result == -1) {
                        perror("target dup2()");
                        err_status = 2;
                        goto exit;
                    }
                }
                // Replace the current process image with a new process image
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
                err_status = 2;
                goto exit;
            default:
                // In the parent process
                // If not background, blocking wait for child's termination
                if (is_background == 0){
                    spawn_pid = waitpid(spawn_pid, &child_status, 0);
                    if (WIFSIGNALED(child_status)){
                        dollar_question = 128 + child_status;
                    }
                    else if (WIFSTOPPED(child_status)) {
                        if (kill(spawn_pid, SIGCONT) < 0) goto exit;
                        if (fprintf(stderr, "Child process %jd stopped. Continuing.\n", (intmax_t) spawn_pid) < 0) goto exit;
                        dollar_exclamation = (char *) (intmax_t) spawn_pid;
                        // Add to background process list
                        waitpid(spawn_pid, &child_status, WNOHANG);
                    }
                    else {
                        dollar_question = child_status;
                    }
                    goto exit;
                }
                else {
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

    pid_t my_pid = getpid();
    errno = 0;

    const char *ps1 = getenv("PS1");
    if (ps1 == NULL) ps1 = "";
    char *line = NULL;
    size_t n = 0;
    for (;;) {

        // Clean up
        getcmd:
        is_background = 0;
        for (int i = 0; i < MAX_LINE; i++) {
            cmd_array[i] = NULL;
            free(cmd_array[i]);
        }
        for (int i = 0; i < 256; i++) {
            in_file_name[i] = NULL;
            out_file_name[i] = NULL;
            free(in_file_name[i]);
            free(out_file_name[i]);
        }
        set_infile = 0; set_outfile = 0;
        line_count = 0;
        free_line_count = 0;

        err_status = 0;
        GetCommands(cmd_array, line, n, ps1);

        // Handle exit command
        if (cmd_array[0] == NULL){ // no command word
            // Go back to get command
            goto getcmd;
        }
        if (strcmp(cmd_array[0], "exit") == 0) {
            if (cmd_array[2] != NULL) {
                fprintf(stderr, "exit: too many arguments\n");
                err_status = -1;
                goto exit;
            }
            if (cmd_array[1] != NULL) {
                long exit_status = strtol(cmd_array[1], NULL, 10);
                if (exit_status > -256 && exit_status < 256){
                    fprintf(stderr, "\nexit\n");
                    err_status = (int) exit_status;
                    goto exit;
                    //exit((int) exit_status);
                }
                else {
                    fprintf(stderr, "exit: argument not an int\n");
                    err_status = -1;
                    goto exit;
                    //exit(-1);
                }
            }
            else {
                fprintf(stderr, "\nexit\n");
                kill(0, SIGINT);
                err_status = dollar_question;
                goto exit;
                //exit(dollar_question);
            }
        }
        ParseCommands(cmd_array, in_file_name, out_file_name);

        ExecuteCommands(cmd_array, in_file_name, out_file_name);
    }

    exit:
    return err_status;
}