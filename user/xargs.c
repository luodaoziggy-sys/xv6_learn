#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"

int main(int argc, char *argv[]){
    char buf[512];
    int n;
    char *cmdargs[MAXARG];
    int base_args;
    int i;

    if(argc < 2){
        fprintf(2, "xargs: missing command\n");
        exit(1);
    }

    base_args = argc - 1;
    while (1)
    {
        n=0;
        while(1){
            char c;
            int r = read(0, &c, 1);
            if(r<=0){
                if(n==0){
                    exit(0);
                }
                break;
            }
            if(c=='\n'){
                break;
            }
            buf[n]=c;
            n++;
            if(n>=sizeof(buf)-1){
                break;
            }

        }
        buf[n]='\0';
        for(i=0; i<base_args; i++){
            cmdargs[i] = argv[i+1];   
        }
        cmdargs[base_args] = buf;
        cmdargs[base_args+1] = 0;
        int pid = fork();
        if(pid ==0){
            exec(cmdargs[0], cmdargs);
            fprintf(2, "xargs: exec %s failed\n", cmdargs[0]);
            exit(1);
        }
        else if(pid > 0){
            wait(0);
        }
        else{
            fprintf(2, "xargs: fork failed\n");
            exit(1);
        }


    }
    exit(0);

}