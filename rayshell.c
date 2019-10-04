#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>    
#include <errno.h>          // errno error
#include <sys/wait.h>       // fork waits
#include <sys/resource.h>   // limits
#include <signal.h>         // signal handling
#include <fcntl.h>          // fifo open
#include <limits.h>         // PATH_MAX 
#include <pwd.h>            // used for get pwuid as backup for getenv

#define HISTORY_LIMIT 100

static char *history[HISTORY_LIMIT] = {NULL}; // history array
static int historyCount = 0;                  // the current index for the history
static int interrupt = 0;                     // global variable to indicate if a signal interrupt is active
static int endOfFile = 1;                     // global variable to indicate end of file

static const char *INPUT_MEMORY_ALLOC_ERROR = "Failed to allocate memory for input\n";
static const char *FORK_ERROR = "Error running command due to an issue with processes";
static const char *COMMAND_NOT_FOUND = ": command or path not found";

void interruptHandler(int sig) {
    signal(sig, SIG_IGN);
    interrupt = 1;
    fprintf(stdout, "\n%s ", "Are you sure you want to quit (y/n)?");
    // flush due to lack of \n
    fflush(stdout);
}

// do nothing
void terminationHandler(int sig) {}

void freeHistory() {
    for (int i = 0; i < historyCount; i++) {
        if (history[i]) {
            free(history[i]);
        }
    }
}

char *readLine(int isTerminalInput) {

    // we dont print this when passing by file as there is no prompt
    if (isTerminalInput)
        write(fileno(stdout), "> ", sizeof(char) * 2);

    int allocatedBytes = 128;
    int currentPosition = 0;
    char *input = malloc(sizeof(char) * allocatedBytes);
    char tempChar;

    // check if memory allocated successfully
    if (!input) {
        write(fileno(stderr), INPUT_MEMORY_ALLOC_ERROR, sizeof(char) * strlen(INPUT_MEMORY_ALLOC_ERROR));
        exit(EXIT_FAILURE);
    }

    while ((endOfFile = read(fileno(stdin), &tempChar, 1)) > 0) {
        // see if return was clicked or newline
        if (tempChar == '\n') {
            input[currentPosition] = '\0';
            break;
        }
        else {
            input[currentPosition] = tempChar;
        }

        // byte count
        currentPosition++;

        // if we are going to run out of memory, realloc
        if (currentPosition >= allocatedBytes) {
            allocatedBytes += 128;
            input = realloc(input, allocatedBytes);
            if (!input) {
                write(fileno(stderr), INPUT_MEMORY_ALLOC_ERROR, sizeof(char) * strlen(INPUT_MEMORY_ALLOC_ERROR));
                exit(EXIT_FAILURE);
            }
        }
    }

    return input;
}

int displayHistory() {

    int historyNumber = 1;
    int current = historyCount;

    do {
        // the iteration starts after the most recent elemement, which is the oldest element in the list
        // if nothing was previously added here, it just iterates until it reaches the beginning and then prints normally
        if (history[current]) {
            fprintf(stdout, " %3d %s \n", historyNumber, history[current]);
            historyNumber++;
        }
        // this allows iteration to wrap around and still maintain the proper order
        current = (current + 1) % HISTORY_LIMIT;
    } while (current != historyCount);

    return EXIT_SUCCESS;
}

int changeDirectory(char *path) {

    if (!path || strcmp("~", path) == 0) {
        char *homeDir = getenv("Home");
        if (!homeDir) {
            // if getenv fails, attempt to retrieve with getpwuid
            struct passwd *info = getpwuid(getuid());
            // this was necessary in cases where there was low memory (10000, a segfault would occur here).
            if(info) {
                homeDir = info->pw_dir;
            }
            else {
                fprintf(stderr, "%s\n", "Error: chdir failed due to memory issue or invalid home variable.");
                return EXIT_FAILURE;
            }
        }
        chdir(homeDir);
    }
    else {
        chdir(path);
    }

    return EXIT_SUCCESS;
}

int limit(char *userInput) {

    char *stringHolder;
    long memoryLimit = strtoul(userInput, &stringHolder, 0); // safer then atol/atoi

    if (stringHolder == userInput || *stringHolder != '\0' || errno == ERANGE) {
        fprintf(stderr, "%s %s %s\n", "Limit:", userInput, "is not a valid memory limit");
        return EXIT_FAILURE;
    }

    struct rlimit old, new;
    getrlimit(RLIMIT_DATA, &old);
    new.rlim_cur = memoryLimit;
    new.rlim_max = old.rlim_max; // do not touch max as it cannot be increased after being reduced
    if (setrlimit(RLIMIT_DATA, &new) == -1) {
        fprintf(stderr, "%s\n", "Limit: Memory allocation failed");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

int runCommand(char *command, char *fifoPath, int fifoPassed) {

    // update history, strdup is needed to get whole string (ex ls -l would return ls otherwise)
    history[historyCount] = strdup(command);
    historyCount = (historyCount + 1) % HISTORY_LIMIT;

    char *p = strtok(command, " ");
    char *firstPipeArr[256];
    char *secondPipeArr[256];
    int fd, isPiped = 0, currentIndex = 0;

    while (p != NULL) {
        if (strcmp("|", p) == 0) {
            // if no fifo included, do not bother executing piped command
            // a specific boolean variable is passed as a null fifo path did not pass properly (did not get read as null ptr)
            if (!fifoPassed) {
                fprintf(stdout, "%s\n", "You must pass a FIFO to use pipes");
                return EXIT_FAILURE;
            }
            firstPipeArr[currentIndex] = NULL;
            currentIndex = 0;
            isPiped = 1;
        }
        else {
            if (isPiped)
                secondPipeArr[currentIndex++] = p;
            else
                firstPipeArr[currentIndex++] = p;
        }
        p = strtok(NULL, " ");
    }

    // null terminating needed for execvp
    if (isPiped) {
        secondPipeArr[currentIndex] = NULL;
    }
    else {
        firstPipeArr[currentIndex] = NULL;
    }

    /* these are done outside the fork because we want the parent process to be modified
    *  modifying only the child is redundant because it is a separate copy.
    *  if the user's input is passed in an incorrect format (i.e., history 100 or limit 100 100, error will be handled below like regular commands)
    */
    if (((strcmp(firstPipeArr[0], "chdir") == 0) || (strcmp(firstPipeArr[0], "cd") == 0)) && !firstPipeArr[2]) {
        changeDirectory(firstPipeArr[1]);
        return (EXIT_SUCCESS);
    }
    else if ((strcmp(firstPipeArr[0], "limit") == 0) && firstPipeArr[1] && !firstPipeArr[2]) {
        limit(firstPipeArr[1]);
        return (EXIT_SUCCESS);
    }

    pid_t pidOne = fork();

    if (pidOne < 0) {
        fprintf(stderr, "%s\n", FORK_ERROR);
        return EXIT_FAILURE;
    }
    else if (pidOne == 0) {
        int error = 0;
        // similarly, if history is entered incorrectly, i.e., history ls or history 100, let execvp handle the error
        if (!isPiped && strcmp(firstPipeArr[0], "history") == 0 && firstPipeArr[1] == NULL) {
            displayHistory();
            exit(EXIT_SUCCESS);
        }
        else if (isPiped) {
            // close stdout, open fd to write to fifo, move stdout to fifo, execute command, close fd
            close(fileno(stdout));
            fd = open(fifoPath, O_WRONLY);
            dup2(fd, fileno(stdout));
            error = execvp(firstPipeArr[0], firstPipeArr);
            close(fd);
        }
        else {
            error = execvp(firstPipeArr[0], firstPipeArr);
        }
        // error check not necessary in testing (only prints during error) but added anyways
        if(error == -1)
            fprintf(stderr, "%s %s\n", firstPipeArr[0], COMMAND_NOT_FOUND);
        exit(EXIT_SUCCESS);
    }

    if (isPiped) {
        pid_t pidTwo = fork();

        if (pidTwo < 0) {
            fprintf(stderr, "%s\n", FORK_ERROR);
            return EXIT_FAILURE;
        }
        else if (pidTwo == 0) {
            // close stdin, set fd to read fifo, set fifo as stdin, execute command, close fd
            close(fileno(stdin));
            fd = open(fifoPath, O_RDONLY);
            dup2(fd, STDIN_FILENO);
            int error = execvp(secondPipeArr[0], secondPipeArr);
            close(fd);
            // error check not necessary in testing (only prints during error) but added anyways
            if(error == -1)
                fprintf(stderr, "%s %s\n", secondPipeArr[0], COMMAND_NOT_FOUND);
            exit(EXIT_SUCCESS);
        }
    }
    wait(NULL);
    wait(NULL); // not a problem having two waits outside since one will be ignored if there is no process

    return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {

    char *fifoPath = NULL;
    char actualpath[PATH_MAX];
    if (argc > 2) {
        fprintf(stdout, "%s\n", "Error: Your input is invalid. You may only enter one argument (fifo path). Otherwise, you may pass a text file by redirection.");
        exit(EXIT_FAILURE);
    }
    else if (argc == 2) {
        // allows getting fifo path even if in different directory
        fifoPath = realpath(argv[1], actualpath);
    }

    // check if input is from file redirection or is coming direct from terminal
    int isTerminalInput = isatty(0);

    // only required to be handled in parent so handled here
    signal(SIGINT, interruptHandler);
    signal(SIGTSTP, terminationHandler);

    do {
        char *userInput = readLine(isTerminalInput);
        if (strlen(userInput) > 0 && !interrupt) {
            runCommand(userInput, fifoPath, (fifoPath != NULL));
        }
        else if (interrupt) {
            /* the purpose of the interrupt variable, as well as handling the signal through the same means as
            regular input is to prevent user response from running runCommand
            without this, after a successful interrupt where you press no, the next interrupt would fail because the "n" would be passed to runCommand (then next would work)
            */
            if (strcmp(userInput, "y") == 0 || strcmp(userInput, "Y") == 0) {
                break;
            }
            signal(SIGINT, interruptHandler);
            interrupt = 0;
        }
        free(userInput);
    } while (endOfFile != 0);

    freeHistory();

    return (EXIT_SUCCESS);
}