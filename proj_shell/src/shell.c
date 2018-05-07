#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/wait.h>

// For now, you can get only 100 characters as the line
// 10 command, and 10 command arguments(arguments of execvp)
// You can change next three constant if you want to change the number of commands.
#define MAX_LINE_SIZE 100
#define MAX_CMD_LIST 100
#define MAX_CMD_ARG 100

void interactive_mode();
void batch_mode(char * pathname);
int parse_line(char * line, char ** cmd_temp, const char * delimeter);
void parse_cmd_temp(char * cmd_temp, char ** cmd_args, const char * delimeter);
char * rtrim(char * s);
char * ltrim(char * s);
char * trim(char * s);

int main(int argc, char *argv[]){

    // Determine the mode by the argument
    if(argc==1){
        interactive_mode();
    }
    else if(argc==2){
        batch_mode(argv[1]);
    }
    else{
        fprintf(stderr, "usage1 : ./shell\n");
        fprintf(stderr, "usage2 : ./shell <batch_file>\n");
        exit(0);
    }

    return 0;
}

void interactive_mode(){

    while(1){
        char line[MAX_LINE_SIZE];
        char * cmd_temp[MAX_CMD_LIST];
        char * cmd_args[MAX_CMD_ARG];
        int num_cmd_temp;

        printf("prompt > ");
        // Get the set of commands from stdin
        if(fgets(line, sizeof(line), stdin) == NULL){
            fprintf(stderr, "fgets error\n");
            exit(0);
        }

        // Parse the line into cmd_temp by ";"
        // cmd_temp may contain " " in its side
        num_cmd_temp = parse_line(line, cmd_temp, ";");

        // Erase " " in cmd_temps' side
        // If one cmd_temp is "quit" -> turn off the program
        for(int i=0 ; i<num_cmd_temp ; i++){
            char * temp = trim(cmd_temp[i]);
            strcpy(cmd_temp[i], temp);
            if(!strcmp(cmd_temp[i], "quit"))
                exit(0);
        }

        pid_t pid[num_cmd_temp];

        // Create child processes num_cmd_temp times
        for(int i=0 ; i<num_cmd_temp ; i++){
            parse_cmd_temp(cmd_temp[i], cmd_args, " ");

            pid[i] = fork();
            if(pid[i] < 0){
                fprintf(stderr, "fork[%d] error\n", i);
                exit(0);
            }
            else if(pid[i] == 0)
                if(execvp(cmd_args[0], cmd_args) < 0)
                    kill(getpid(), SIGINT);
        }
        for(int i=0 ; i<num_cmd_temp ; i++){
            int status;
            waitpid(pid[i], &status, 0);
        }
    }
}

void batch_mode(char * pathname){

    // Open file by the pathname
    FILE * fp = fopen(pathname, "rt");
    if(fp == NULL){
        fprintf(stderr, "file open error\n");
        exit(0);
    }

    while(1){
        char line[MAX_LINE_SIZE];
        char * cmd_temp[MAX_CMD_LIST];
        char * cmd_args[MAX_CMD_ARG];
        int num_cmd_temp;

        // Get the set of commands from file
        if(fgets(line, sizeof(line), fp) == NULL){
            fprintf(stderr, "fgets error\n");
            exit(0);
        }
        // Echo the line from the batch_file
        printf("%s", line);

        // Parse the line into cmd_temp by ";"
        // cmd_temp may contain " " in its side
        num_cmd_temp = parse_line(line, cmd_temp, ";");

        // Erase " " in cmd_temps' side
        // If one cmd_temp is "quit" -> turn off the program
        for(int i=0 ; i<num_cmd_temp ; i++){
            char * temp = trim(cmd_temp[i]);
            strcpy(cmd_temp[i], temp);
            if(!strcmp(cmd_temp[i], "quit"))
                exit(0);
        }

        pid_t pid[num_cmd_temp];

        // Create child processes num_cmd_temp times
        for(int i=0 ; i<num_cmd_temp ; i++){
            parse_cmd_temp(cmd_temp[i], cmd_args, " ");

            pid[i] = fork();
            if(pid[i] < 0){
                fprintf(stderr, "fork[%d] error\n", i);
                exit(0);
            }
            else if(pid[i] == 0)
                if(execvp(cmd_args[0], cmd_args) < 0)
                    kill(getpid(), SIGINT);
        }
        for(int i=0 ; i<num_cmd_temp ; i++){
            int status;
            waitpid(pid[i], &status, 0);
        }
    }
}

// Parse the line into cmd_temp by given delimeter
// Return the number of cmd_temps parsed by the line
int parse_line(char * line, char ** cmd_temp, const char * delimeter){

    int num_cmd_temp = 0;

    // If the line is the character 'n' / when the input is only enter key
    // -> return 0(the number of cmd_temp is 0
    if(!strcmp(line, "\n"))
        return 0;

    // Change line's last character '\n' into ' '
    line[strlen(line)-1] = ' ';
    // Erase the space(' ') in line's side
    line = trim(line);

    cmd_temp[num_cmd_temp] = strtok(line, delimeter);
    while(cmd_temp[num_cmd_temp] != NULL)
        cmd_temp[++num_cmd_temp] = strtok(NULL, delimeter);

    return num_cmd_temp;
}

// Parse the cmd_temp into cmd_args by given delimeter
void parse_cmd_temp(char * cmd_temp, char ** cmd_args, const char * delimeter){

    int i = 0;

    cmd_args[i] = strtok(cmd_temp, delimeter);

    while(cmd_args[i] != NULL)
        cmd_args[++i] = strtok(NULL, delimeter);

    return;
}

// Reference : http://mwultong.blogspot.com/2007/06/c-trim-ltrim-rtrim.html
// I refer these codes(rtrim, ltrim, trim) from the website above
char* rtrim(char* s) {
    char t[1024];
    char *end;

    strcpy(t, s);
    end = t + strlen(t) - 1;
    while (end != t && isspace(*end))
        end--;
    *(end + 1) = '\0';
    s = t;

    return s;
}

char* ltrim(char *s) {
    char* begin;
    begin = s;

    while (*begin != '\0') {
        if (isspace(*begin))
            begin++;
        else {
            s = begin;
            break;
        }
    }

    return s;
}

char* trim(char *s) {
    if(s == NULL)
        return NULL;
    return rtrim(ltrim(s));
}
