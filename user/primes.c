#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void primes(int p[2]){
    close(p[1]);
    int prime;
    if(read(p[0], &prime, 4) == 0){
        close(p[0]);
        exit(0);
    }
    printf("prime: %d\n", prime);
    int p2[2];
    pipe(p2);
    int pid = fork();
    if(pid == 0){
        primes(p2);
    } else {
        int n;
        while(read(p[0], &n, 4) > 0){
            if(n % prime != 0){
                write(p2[1], &n, 4);
            }
        }
        close(p[0]);
        close(p2[1]);
        close(p2[0]);
        wait(0);

    }
    exit(0);
}

int main(){
    int p[2];
    pipe(p);
    int i;
    for(i = 2; i <= 35; i++){
        write(p[1], &i, 4);
    }
    primes(p);
    close(p[1]);
    exit(0);
}