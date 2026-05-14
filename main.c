#include <errno.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#define FIFONAME "/home/tatovka/logger_fifo"
#define PROTONAME "/home/tatovka/logger_proto"
#define FAILURE_CODE 1
#define KILL_CODE 2
#define ALARM_FREQ 5

#define BECOME_DAEMON become_daemon && !is_daemon

volatile sig_atomic_t should_break = 0;
volatile sig_atomic_t last_sig;
volatile sig_atomic_t was_alarm = 0;
volatile sig_atomic_t become_daemon = 0;
volatile sig_atomic_t is_daemon = 0;

struct logger_stat {
    int cycles;
    int size;
    int alarms; 
} logger_stat = {0, 0, 0};

void leave(int code) {
    printf("%s", strerror(errno));
    exit(code);
}

void print_stat() {
    printf("read %i messages of %i bytes, got %i alarm signals\n", 
        logger_stat.cycles, logger_stat.size, logger_stat.alarms);
}

void daemonize()
{   
    pid_t pid, sid;
    if ((pid = fork()) < 0)
    {perror ("Fork 1"); exit(EXIT_FAILURE); }
    if (pid > 0) exit(EXIT_SUCCESS); // Родитель сразу завершается
    // Контроль над создаваемыми файлами
    umask(0);
    // Стать лидером в группе процессов (это за кадром)
    if ((sid = setsid()) < 0)
    {perror ("Setsid"); exit(EXIT_FAILURE); }
    // Второй fork, полная отвязка от терминала
    if ((pid = fork()) < 0)
    {perror ("Fork 2 failure"); exit(EXIT_FAILURE); }
    if (pid > 0) exit(EXIT_SUCCESS);
    // Отвязка от каталога
    if ((chdir("/")) < 0) {perror ("Chdir"); exit(EXIT_FAILURE);}

    close(STDIN_FILENO);
    FILE* openRes = fopen(PROTONAME, "w"); 
    if (openRes == NULL) {
        perror("failed to open proto file");
        exit(EXIT_FAILURE);
    }

    close(STDOUT_FILENO); close(STDERR_FILENO);
    dup(STDIN_FILENO); dup(STDIN_FILENO);
    printf("Became a demon\n");
    print_stat();
    alarm(ALARM_FREQ);
    is_daemon = 1;
}

void sig_handler(int signum) {
    switch (signum) {
        case SIGINT : {
            should_break = 1;
            last_sig = SIGINT;
            break;
        }
        case SIGTERM : {
            should_break = 1;
            last_sig = SIGTERM;
            break;
        }
        case SIGQUIT : {
            break;
        }
        case SIGUSR1 : {
            print_stat();
            break;
        }
        case SIGALRM : {
            was_alarm = 1;
            break;
        }
        case SIGHUP : {
            become_daemon = 1;
            break;
        }
        default : {
            perror("invalid signum for sig_handler\n");
        }
    }
}

void check_alarm() {
    if (!was_alarm) return;
    was_alarm = 0;

    time_t rawtime;
    struct tm * timeinfo;
    time ( &rawtime );
    timeinfo = localtime ( &rawtime );
    static char buffer[64];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", timeinfo);
    printf("%s - logger is working\n", buffer);
    logger_stat.alarms += 1;
    alarm(ALARM_FREQ);
}

void syscall_err() {
    if (errno != EINTR) {
        leave(FAILURE_CODE);
    }
    if (last_sig == SIGTERM) {
        printf("SIGTERM recieved\n");
        print_stat();
        exit(KILL_CODE); 
    } 
    if (last_sig == SIGINT) {
        printf("SIGINT recieved\n");
    }
    if (BECOME_DAEMON) daemonize();
    check_alarm();
}

void init_syscalls() {
    static struct sigaction my_sig_handler = {.sa_handler = sig_handler};
    if (sigaction(SIGTERM, &my_sig_handler, NULL) < 0) {
        perror("failed to set SIGTERM handler");
        exit(FAILURE_CODE);
    }
    if (sigaction(SIGINT, &my_sig_handler, NULL) < 0) {
        perror("failed to set SIGINT handler");
        exit(FAILURE_CODE);
    }
    if (sigaction(SIGQUIT, &my_sig_handler, NULL) < 0) {
        perror("failed to set SIGQUIT handler");
        exit(FAILURE_CODE);
    }
    if (sigaction(SIGUSR1, &my_sig_handler, NULL) < 0) {
        perror("failed to set SIGUSR1 handler");
        exit(FAILURE_CODE);
    }
    if (sigaction(SIGALRM, &my_sig_handler, NULL) < 0) {
        perror("failed to set SIGALRM handler");
        exit(FAILURE_CODE);
    }
    if (sigaction(SIGHUP, &my_sig_handler, NULL) < 0) {
        perror("failed to set SIGHUP handler");
        exit(FAILURE_CODE);
    }
}

int main(int argc, char** argv) {
    int shouldDaemonize = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-d") == 0) shouldDaemonize = 1;
    }
    
    init_syscalls();

    
    int mkres = mkfifo(FIFONAME, 0600);
    if (mkres < 0) {
        if (errno != EEXIST)  leave(FAILURE_CODE);
        struct stat fifoStat;
        if (stat(FIFONAME, &fifoStat) < 0) leave(FAILURE_CODE);
        if (!S_ISFIFO(fifoStat.st_mode)) leave(FAILURE_CODE);
    }

    if (shouldDaemonize) daemonize();
    else alarm(ALARM_FREQ);
    while (!should_break) {
        if (BECOME_DAEMON) daemonize();

        int fifoD = open(FIFONAME, O_RDONLY);
        if (fifoD < 0) {
            syscall_err();
            continue;
        }
        check_alarm();

        int s, S = 0;
        #define BUFSZ 1024
        char buf[BUFSZ + 2];
        do {
            s = read(fifoD, buf + S, BUFSZ - S);
            if (s < 0) {
                syscall_err();
                s = 1; // s > 0 is true
                continue;
            }
            if (BECOME_DAEMON) daemonize();
            check_alarm();
            S += s;
        } while (s > 0);

        if (close(fifoD) < 0) {
            perror("failed to close fifo\n");
            return FAILURE_CODE;
        }

        if (buf[S - 1] != '\n') {
            buf[S] = '\n';
            buf[S + 1] = 0;
        } else {
            buf[S] = 0;
        }

        printf("%s", buf);
        logger_stat.cycles += 1;
        logger_stat.size += S;
    }
    print_stat();
    if (remove(FIFONAME) < 0) {
        perror("failed to remove fifo file");
        return FAILURE_CODE;
    }
    return 0;
}