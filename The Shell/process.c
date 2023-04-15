#include "process.h"

//[GLOBAL] Template for temporary files
char template[] = "./tempXXXXXX";
//[GLOBAL] Status environment variable
const char* env_var = "?";
const char* dir_var = "PWD";


// Directory node struct
typedef struct dir{
    char* path;
    struct dir* prev;
    struct dir* next;
} dir;
// Directory stack metadata
typedef struct file_sys{
    dir* head;
    dir* tail;
}file_sys;

// MY FUNCTIONS
void zombies(int* status);
int handle_pipeline(const CMD *cmdList);
int handle_simple(const CMD *cmdList);
int handle_redirections(const CMD *cmdList);
int handle_sep_and(const CMD *cmdList);
int handle_sep_or(const CMD *cmdList);
int handle_subcommand(const CMD *cmdList);
int handle_sep_end_BG(const CMD *cmdList, int flag);
file_sys* file_sys_init();
int handle_builtins(const CMD *cmdList);
void file_sys_destroy(file_sys* metadata);
int cd(file_sys* dir_list, char* target_directory);
int popd(file_sys* dir_list);
int pushd(file_sys* dir_list, char* target_directory);

// [GLOBAL] Directory Stack
file_sys* global_file_sys = NULL;

/**
 * Main processing function
*/
int process (const CMD *cmdList){
    int type, status_id, status;

    type = cmdList->type;

    switch (type)
    {
    case SIMPLE:
        status_id = handle_simple(cmdList);
        break;
    
    case PIPE:
        status_id = handle_pipeline(cmdList);
        break;
    
    case SEP_AND:
        status_id = handle_sep_and(cmdList);
        break;
    
    case SEP_OR:
        status_id = handle_sep_or(cmdList);
        break;

    case SEP_BG:
        status_id = handle_sep_end_BG(cmdList, 0);
        break;
        
    case SEP_END:
        status_id = handle_sep_end_BG(cmdList, 0);
        break;

    case SUBCMD:
        status_id = handle_subcommand(cmdList);
        break;

    default:
        break;
    }

    // Reap Zombie processes
    zombies(&status);
    return status_id;
}

/**
 * Handles execution of a pipeline
*/
int handle_pipeline(const CMD *cmdList){
    int left_status, right_status, err_no;
    int pipe_fd[2];
    char buff[10];
    // Pipe fd array
    int ret;
    // Create pipe(s)
    ret = pipe(pipe_fd);
    if (ret == -1)
    {
        err_no = errno;
        perror("pipe(): Pipe failed");
        return err_no;
    }
    // FORK...if (dup1) else FORK...if(DUP2) ELSE (WAIT---ACTUAL PARENT)
    // Left subshell
    int pid_1 = fork();
    if (pid_1 == -1) // ERROR
    {
        err_no = errno;
        perror("fork(): Fork failed");
        exit(err_no);
    }
    else if(pid_1 == 0) // CHILD
    {
        // Overwrite stdout
        dup2(pipe_fd[1], STDOUT_FILENO);
        close(pipe_fd[0]);
        close(pipe_fd[1]);
        exit(process(cmdList->left));
    }
    else // PARENT
    {
        // Right subshell
        int pid_2 = fork();
        if (pid_2 == -1) // ERROR
        {
            err_no = errno;
            perror("fork(): Fork failed");
            exit(err_no);
        }
        else if (pid_2 == 0) // CHILD
        {
            // Overwrite stdin
            dup2(pipe_fd[0], STDIN_FILENO);
            close(pipe_fd[0]);
            close(pipe_fd[1]);
            exit(process(cmdList->right));
        }
        else // REAL PARENT
        {
            close(pipe_fd[0]);
            close(pipe_fd[1]);
            // Parent waits for child to finish
            waitpid(pid_1, &left_status, 0);
            waitpid(pid_2, &right_status, 0);
            // Update status of both children
            left_status = STATUS(left_status);
            right_status = STATUS(right_status);
        }
    }
    // Return status of rightmost child to fail
    if (right_status != 0)
    {
        // Set Status Environment variable
        sprintf(buff, "%d", right_status);
        setenv(env_var, buff, 1);
        return right_status;
    }
    else // Return 0 (every stage is true)
    {
        // Set Status Environment variable
        sprintf(buff, "%d", left_status);
        setenv(env_var, buff, 1);
        return left_status;
    }
}

/**
 * Handles the execution of a simple command
*/
int handle_simple(const CMD *cmdList){
    int pid, status, err_no, ret, final_status;
    char buff[10];
    // Name of file to be executed
    char* commandFile = cmdList->argv[0];
    // Handle builtin commands
    if(strcmp(commandFile, "cd") == 0 || strcmp(commandFile, "pushd") == 0 || strcmp(commandFile, "popd") == 0)
    {
       err_no = handle_builtins(cmdList);
       return err_no;
    }
    // Fork child
    pid = fork();
    if (pid < 0) // Fork failed
    {
        err_no = errno;
        perror("fork()");
        exit(err_no);
    }
    else if (pid == 0) // Child (New process)
    {
        // Set local environment variables
        for (size_t i = 0; i < cmdList->nLocal; i++)
        {
            // Ignore error returned by "set env"
            setenv(cmdList->locVar[i], cmdList->locVal[i], 1);
        }
        // Handle REDIRECTIONS if present
        status = handle_redirections(cmdList);
        // Run file executable
        ret = execvp(commandFile, cmdList->argv);
        if (ret == -1)
        {
            err_no = errno;
            perror("execvp()");
            exit(1);
        }
    }
    else // Parent path (main)
    {
        // Parent waits for child to finish
        waitpid(pid, &status, 0);
        final_status = STATUS(status);
        // Set Status Environment variable
        sprintf(buff, "%d", final_status);
        setenv(env_var, buff, 1);
    }
    return final_status;
}


/**
 * Handles all REDIRECTION variations in a simple command
*/
int handle_redirections(const CMD *cmdList){
    int new_stdout_fd, new_stdin_fd;
    int err_no = 0;
    // RED_OUT
    if (cmdList->toType == RED_OUT)
    {
        // Open new file (toFile)
        new_stdout_fd = open(cmdList->toFile, O_CREAT | O_WRONLY | O_TRUNC, S_IRWXU);
        // Error by open()
        if (new_stdout_fd == -1)
        {
            err_no = errno;
            perror("open()");
            exit(err_no);
        }
        // Overwrite STDOUT with new_stdout_fd
        dup2(new_stdout_fd, STDOUT_FILENO);
        // Close opened file
        close(new_stdout_fd);
    }
    // RED_OUT_APP
    if(cmdList->toType == RED_OUT_APP)
    {
        // Open "toFile" with "O_APPEND" flag
        new_stdout_fd = open(cmdList->toFile, O_WRONLY | O_APPEND | O_CREAT, S_IRWXU);
        // Error by open()
        if (new_stdout_fd == -1)
        {
            err_no = errno;
            perror("open()");
            exit(err_no);
        }
        // Overwrite STDOUT with new_stdout_fd
        dup2(new_stdout_fd, STDOUT_FILENO);
        // Close opened file
        close(new_stdout_fd);
    }
    // RED_IN
    if (cmdList->fromType == RED_IN)
    {
        // Open new file (toFile)
        new_stdin_fd = open(cmdList->fromFile, O_RDONLY, S_IRWXU);
        // Error by open()
        if (new_stdin_fd == -1)
        {
            err_no = errno;
            perror("open()");
            exit(err_no);
        }
        // Overwrite STDIN with new_stdin_fd
        dup2(new_stdin_fd, STDIN_FILENO);
        // Close opened file
        close(new_stdin_fd);
    }
    // RED_IN_HERE
    if (cmdList->fromType == RED_IN_HERE)
    {
        new_stdin_fd = mkstemp(template);
        // Error by mkstemp()
        if (new_stdin_fd == -1)
        {
            err_no = errno;
            perror("mkstemp()");
            exit(err_no);
        }
        // Copy contents of "fromFile" to new temp file
        write(new_stdin_fd, cmdList->fromFile, strlen(cmdList->fromFile));
        // Reposition file cursor to the start of the file
        lseek(new_stdin_fd, 0, SEEK_SET);
        // Overwrite STDIN with new_stdin_fd
        dup2(new_stdin_fd, STDIN_FILENO);
        // Close opened file
        close(new_stdin_fd);
    }
    return err_no;
}

/**
 * Handles SEP_AND command operator
*/
int handle_sep_and(const CMD *cmdList){
    // Process left child
    int left_status, right_status;
    left_status = process(cmdList->left);
    // Decide if to process right child depending on left child return status
    if (left_status == 0)
    {
        right_status = process(cmdList->right);
        return right_status;
    }
    return left_status;
}

/**
 * Handles SEP_OR command operator
*/
int handle_sep_or(const CMD *cmdList){
    // Process left child
    int left_status, right_status;
    left_status = process(cmdList->left);
    // Decide if to process right child depending on left child return status
    if (left_status != 0)
    {
        right_status = process(cmdList->right);
        return right_status;
    }
    return left_status;
}

/**
 * Handles SUBCMD
*/

int handle_subcommand(const CMD *cmdList){
    int pid, status, final_status, err_no;
    char buff[10];
    // Fork child
    pid = fork();
    if (pid < 0) // Fork failed
    {
        err_no = errno;
        perror("fork()");
        exit(err_no);
    }
    else if (pid == 0) // Child process
    {
        err_no = handle_redirections(cmdList);
        exit(process(cmdList->left));
    }
    else // Parent path (main)
    {
        // Parent waits for child to finish
        waitpid(pid, &status, 0);
        final_status = STATUS(status);
        // Set Status Environment variable
        sprintf(buff, "%d", final_status);
        setenv(env_var, buff, 1);
    }
    return final_status;
}
/**
 * Handles SEP_END and SEP_AND command terminators
*/
int handle_sep_end_BG(const CMD *cmdList, int flag){
    int type, left_type, ret, pid, pid2, err_no;
    
    type = cmdList->type;
    switch (type)
    {
    case SEP_BG:
        left_type = cmdList->left->type;
        if (left_type == SEP_BG || left_type == SEP_END)
        {
            handle_sep_end_BG(cmdList->left, 1);
        }
        else
        {
            // Bakgrounded Process
            pid = fork();
            if (pid < 0)
            {
                err_no = errno;
                perror("fork()");
                exit(err_no);
            }
            else if (pid == 0) // Child process
            {
                exit(process(cmdList->left));
            }
            else // Parent
            {
                fprintf(stderr, "Backgrounded: %d\n", pid);
            }   
        }
        break;

    case SEP_END:
        left_type = cmdList->left->type;
        if (left_type == SEP_BG || left_type == SEP_END)
        {
            handle_sep_end_BG(cmdList->left, 0);
        }
        else
        {
            // PROCESS
            ret = process(cmdList->left);
        }
        break;

    default:
        break;
    }
    // Handle Right child
    // if flag, BACKGROUND, else RUN then DONE
    if (flag)
    {
        // Background right child ELSE run right child
        pid2 = fork();
        if (pid2 < 0) // Error
        {
            err_no = errno;
            perror("fork()");
            exit(err_no);
        }
        else if (pid2 == 0) // Child
        {
            exit(process(cmdList->right));
        }
        else // Parent
        {
            fprintf(stderr, "Backgrounded: %d\n", pid2);
        }
    }
    else
    {
        // RUN command
        ret = process(cmdList->right);
    }
    return ret;
}

/**
 * Reap zombie processes
*/
void zombies(int* status){
    int pid;

    do
    {
        pid = waitpid(-1, status, WNOHANG);
        if(pid <= 0){
            break;
        }
        fprintf(stderr, "Completed: %d (%d)\n", pid, (*status));
    }
    while(1);
}
/**
 * Initaialize the file system directory
*/
file_sys* file_sys_init(){
    // Allocate space for a new file system linked list metadata
    file_sys* new_file_sys = malloc(sizeof(file_sys));
    // Initialize the new file system
    new_file_sys->head = NULL;
    new_file_sys->tail = NULL;

    return new_file_sys;
}

/**
 * Create a dir node
*/
dir* dir_init(char* path){
    // Allocate space for new node
    dir* new_dir = malloc(sizeof(dir));
    // Duplicate path name
    char* path2 = malloc(sizeof(char) * strlen(path));
    strcpy(path2, path);
    // Initialize dir node
    new_dir->path = path2;
    new_dir->next = NULL;
    new_dir->prev = NULL;

    return new_dir;
}

/**
 * Destroy a dir node
*/
void dir_destroy(dir* node){
    // Free path string
    free(node->path);
    // Free actual node
    free(node);
}

/**
 * Destroy a file_sys struct
*/
void file_sys_destroy(file_sys* metadata){
    // Free nodes in linked list
    dir* ptr = metadata->head;
    while (ptr != NULL)
    {
        dir* temp = ptr->next;
        dir_destroy(ptr);
        ptr = temp;
    }
    // Free actual metadata struct
    free(metadata);
}

/**
 * Print absolute paths on the stack
*/
int file_sys_print(file_sys* metadata){
    int err_no = 1;
    dir* ptr = metadata->head;
    char curr_directory[PATH_MAX];
    // Get current working directory (TARGET DIRECTORY)
    char* ret2 = getcwd(curr_directory, PATH_MAX);
    if (ret2 == NULL)
    {
        err_no = errno;
        perror("getcwd()");
        return err_no;
    }
    fprintf(stdout, "%s", curr_directory);
    while (ptr != NULL)
    {
        fprintf(stdout, " %s", ptr->path);
        ptr = ptr->prev;
    }
    fprintf(stdout, "\n");

    return err_no;
}
/**
 * CD built-in command
*/
int cd(file_sys* dir_list, char* target_directory){
    int ret, err_no;
    // Buffer to store current path
    char current_dir[PATH_MAX];
    // Move to target directory
    ret = chdir(target_directory);
    if (ret != 0)
    {
        err_no = errno;
        perror("cd: chdir()");
        return (err_no);
    }
    // Get current working directory
    char* ret2 = getcwd(current_dir, PATH_MAX);
    if (ret2 == NULL)
    {
        err_no = errno;
        perror("getcwd()");
        return err_no;
    }
    // Set $PWD local environment variable
    setenv(dir_var, current_dir, 1);
    return ret;
}

/**
 * Pop built-in command
*/
int popd(file_sys* dir_list){
    int ret, err_no;
    dir* old_top = dir_list->head;
    // Remember second node
    if (old_top->prev == NULL)
    {
        dir_list->head = NULL;
        dir_list->tail = NULL;
    }
    else
    {
        dir* new_top = dir_list->head->prev;
        // Set new top
        dir_list->head = new_top;
    }
    // Change directory to new top node directory
    ret = chdir(old_top->path);
    if (ret != 0)
    {
        err_no = errno;
        perror("cd: chdir()");
        return (err_no);
    }
    // Set $PWD local environment variable
    setenv(dir_var, old_top->path, 1);
    err_no = file_sys_print(dir_list);
    // Destroy top node
    dir_destroy(old_top);

    return err_no;
}

void push_assist(file_sys* dir_list, dir* new_dir){
    // Attach node to linked list
    if(dir_list->head != NULL)
    {
        dir_list->head->next = new_dir;
        new_dir->prev = dir_list->head;
        dir_list->head = new_dir;
    }
    else
    {
        dir_list->head = new_dir;
        dir_list->tail = new_dir;
    }
}
/**
 * Push built-in command
*/
int pushd(file_sys* dir_list, char* target_directory){
    int err_no;
    // Buffer to store current and target directory paths
    char current_dir[PATH_MAX];
    char target_dir[PATH_MAX];
    // Get current working directory
    char* ret = getcwd(current_dir, PATH_MAX);
    if (ret == NULL)
    {
        err_no = errno;
        perror("getcwd()");
        return err_no;
    }
    // Create a node for the current directory
    dir* curr_dir = dir_init(current_dir);
    // Push current directory to stack
    push_assist(dir_list, curr_dir);
    // Move to target directory
    int ret1 = chdir(target_directory);
    if (ret1 != 0)
    {
        err_no = errno;
        perror("cd: chdir()");
        return err_no;
    }
    // Get current working directory (TARGET DIRECTORY)
    char* ret2 = getcwd(target_dir, PATH_MAX);
    if (ret2 == NULL)
    {
        err_no = errno;
        perror("getcwd()");
        return err_no;
    }
    // Set $PWD local environment variable
    setenv(dir_var, target_dir, 1);
    // Print out directory stack
    err_no = file_sys_print(dir_list);

    return err_no;
}

/**
 * Handle builtin commands
*/
int handle_builtins(const CMD *cmdList){
    int err_no;
    // Built-in commands [cd, pushd, popd]
    char* command = cmdList->argv[0];
    char* target_directory = cmdList->argv[1];
    int arguments = cmdList->argc;
    // Initialize directory linked list
    if(global_file_sys == NULL)
    {
        global_file_sys = file_sys_init();
    }
    if (strcmp(command, "cd") == 0) 
    {
        // Insufficient arguments
        if(arguments > 2)
        {
            fprintf(stderr, "cd(): Usage - cd <directory>\n");
        }
        else if (arguments == 1)
        {
            // Change directory to home directory
            char* home = getenv("HOME");
            err_no = cd(global_file_sys, home);
        }
        else
        {
            err_no = cd(global_file_sys, target_directory);
        }
    }
    else if (strcmp(command, "popd") == 0)
    {
        // Empty directory stack
        if (global_file_sys->head == NULL)
        {
            fprintf(stderr, "popd(): Directory stack is empty\n");
        }
        else
        {
            err_no = popd(global_file_sys);
        }
    }
    else if (strcmp(command, "pushd") == 0)
    {
        // Insufficient arguments
        if(arguments != 2)
        {
            fprintf(stderr, "pushd(): Usage - pushd <directory>\n");
        }
        else
        {
            err_no = pushd(global_file_sys, target_directory);
        }
    }
    // Clean up
    //file_sys_destroy(global_file_sys);
    return err_no;
}