
#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <string.h>

int clockShmid;
int clockSem;
int pcbShmid;
int resourceShmid;
int clockSem;

struct sembuf sem;

struct PCB
{
    int simPid;
    int processPid;
    int actualPid;
    int inQueue;
    int deadlocked;
    int terminated;
    int typeResourceRequested;
    int numberOfResourceRequested;
    int* allocated[20];
};
struct PCB* sharedPcb;

struct Resource
{
    int totalResource;
    int allocatedResource;
    int availableResource;
    int shareable;
};
struct Resource* sharedResource;

struct Clock
{
    long seconds;
    long nanoSeconds;
};

void semLock();
void semRelease();

int main(int argc, char *argv[0])
{
    key_t semClockKey = ftok("oss", 1);
    clockSem = semget(semClockKey, 1, 0666);
    if(clockSem == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    semctl(clockSem, 0, SETVAL, 1);

    key_t pcbKey = ftok("oss", 2);
    pcbShmid = shmget(pcbKey, sizeof(struct PCB) * 18, 0666);
    if(pcbShmid == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        semctl(clockSem, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }

    sharedPcb = shmat(pcbShmid, NULL, 0);
    if(sharedPcb == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }

    key_t clockKey = ftok("oss", 3);
    clockShmid = shmget(clockKey, sizeof(struct Clock), 0666);
    if(clockShmid == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }

    struct Clock* sharedClock = shmat(clockShmid, NULL, 0);
    if(sharedClock == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        shmctl(clockShmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    key_t resourceKey = ftok("oss", 4);
    resourceShmid = shmget(resourceKey, sizeof(struct Resource) * 20, 0666);
    if(resourceShmid == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        shmctl(clockShmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    sharedResource = shmat(resourceShmid, NULL, 0);
    if(sharedResource == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        shmctl(clockShmid, IPC_RMID, NULL);
        shmctl(resourceShmid, IPC_RMID, NULL);
        exit(EXIT_FAILURE);
    }

    srand(getpid());
    int simPid = atoi(argv[1]);
    sharedPcb[simPid].actualPid = getpid();

    int i;
    int request;
    int possibleAmount;
    int allocated;

    while(1)
    {
        if(rand() % 5 == 0)
        {
            sharedPcb[simPid].terminated = 1;
            return 0;
        }

        if(rand() % 2 == 0)
        {
            request = rand() % 20;
            sharedPcb[simPid].typeResourceRequested = request;
            allocated = sharedPcb[simPid].allocated[request];
            possibleAmount = sharedResource[request].totalResource - allocated;
            sharedPcb[simPid].numberOfResourceRequested = (rand() % possibleAmount) + 1;
            while(sharedPcb[simPid].numberOfResourceRequested != 0);
        }

        else
        {
            for(i = 0; i < 20; i++)
            {
                if(sharedPcb[simPid].allocated[i] != 0)
                {
                    sharedResource[i].availableResource = sharedResource[i].availableResource + sharedPcb[simPid].allocated[i];
                    sharedPcb[simPid].allocated[i] = 0;
                    break;
                }
            }
        }
    }
}

void semLock()
{
    sem.sem_num = 0;
    sem.sem_op = -1;
    sem.sem_flg = 0;
    semop(clockSem, &sem, 1);
}

void semRelease()
{
    sem.sem_num = 0;
    sem.sem_op = 1;
    sem.sem_flg = 0;
    semop(clockSem, &sem, 1);
}
