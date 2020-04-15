#include <stdio.h>
#include <stdlib.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/sem.h>
#include <sys/types.h>
#include <string.h>
#include <signal.h>

int bitmap[1];
int activeChildren = 0;
int clockShmid;
int clockSem;
int pcbShmid;
int resourceShmid;
int numberInQueue = 0;
int requestsGranted = 0;

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

//struct for queue nodes
struct node
{
    int pid;
    struct node* next;
};

struct queue
{
    struct node* first;
    struct node* last;
};

void ctrlc_handler(int signum)
{
    fprintf(stderr, "\n^C interrupt received.\n");
    shmctl(pcbShmid, IPC_RMID, NULL);
    shmctl(resourceShmid, IPC_RMID, NULL);
    shmctl(clockShmid, IPC_RMID, NULL);
    semctl(clockSem, 0, IPC_RMID);
    kill(0, SIGKILL);
}

void createQueue(struct queue* que);
void enqueue(struct queue* que, int pid);
int dequeue(struct queue* que);
int isEmpty(struct queue* que);
void queueRequests(struct queue* que);
void grantRequests(struct queue* que);
int deadlockCheck();
void deadlockRecovery();
void semLock();
void semRelease();

int main(int argc, char *argv[0])
{
    int opt;
    bitmap[0] = 0;
    int simPid;
    int verbose = 0;

    signal(SIGINT, ctrlc_handler);

    while ((opt = getopt(argc, argv, "hv")) != -1)
    {
        switch (opt)
        {
            case 'h':
                printf("Usage: %s [-h][-v]\n", argv[0]);
                printf("        v - turns on verbose logging]n");
                exit(EXIT_FAILURE);
            case 'v':
                verbose = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-h]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    key_t semClockKey = ftok("oss", 1);                   //get a key for semaphore
    clockSem = semget(semClockKey, 1, IPC_CREAT | 0666);      //creates the shared semaphore
    if (clockSem == -1)
    {
        fprintf(stderr, "%s: Error: ", argv[0]);
        perror("");
        exit(EXIT_FAILURE);
    }

    semctl(clockSem, 0, SETVAL, 1);          //set the value of semaphore to 1

    key_t pcbKey = ftok("oss", 2);
    pcbShmid = shmget(pcbKey, sizeof(struct PCB) * 18, IPC_CREAT | 0666);
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
    clockShmid = shmget(clockKey, sizeof(struct Clock), IPC_CREAT | 0666);
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
    resourceShmid = shmget(resourceKey, sizeof(struct Resource) * 20, IPC_CREAT | 0666);
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

    sharedClock->seconds = 0;
    sharedClock->nanoSeconds = 0;
    struct queue* resourceQueue;
    resourceQueue = malloc(sizeof(struct queue));
    createQueue(resourceQueue);
    srand(getpid());

    int i;
    for(i = 0; i < 20; i++)
    {
        sharedResource[i].totalResource = (rand() % 10) + 1;
        sharedResource[i].allocatedResource = 0;
        sharedResource[i].availableResource = sharedResource[i].totalResource;
        if(rand() % 5 == 0)
        {
            sharedResource[i].shareable = 1;
        }
        else
        {
            sharedResource[i].shareable = 0;
        }
    }

    for(i = 0; i < 18; i++)
    {
        sharedPcb[i].inQueue = 0;
        sharedPcb[i].deadlocked = 0;
        sharedPcb[i].terminated = 0;
        sharedPcb[i].typeResourceRequested = -1;
    }

    int totalProcesses = 0;
    int isDeadlocked;

    while(totalProcesses < 100 || activeChildren > 0)
    {
        if(totalProcesses < 100)
        {
            int i;
            for(i = 0; i < 18; i++)
            {
                waitpid(-1, NULL, WNOHANG);

                if(bitmap[0] & (1 << i))
                {
                    continue;
                }
                else
                {
                    bitmap[0] |= (1 << i);
                    sharedPcb[i].simPid = i;
                    sharedPcb[i].processPid = totalProcesses;
                    sharedPcb[i].actualPid = 0;
                    sharedPcb[i].inQueue = 0;
                    sharedPcb[i].deadlocked = 0;
                    sharedPcb[i].terminated = 0;
                    sharedPcb[i].typeResourceRequested = -1;
                    sharedPcb[i].numberOfResourceRequested = 0;
                    int j;
                    for(j = 0; j < 20; j++)
                    {
                        sharedPcb[i].allocated[j] = 0;
                    }

                    pid_t processPid = fork();
                    if(processPid == -1)
                    {
                        fprintf(stderr, "%s: Error: ", argv[0]);
                        perror("");
                        shmdt(sharedClock);
                        shmdt(sharedPcb);
                        shmdt(sharedResource);
                        shmctl(clockShmid, IPC_RMID, NULL);
                        shmctl(pcbShmid, IPC_RMID, NULL);
                        shmctl(resourceShmid, IPC_RMID, NULL);
                        semctl(clockSem, 0, IPC_RMID, NULL);
                        exit(EXIT_FAILURE);
                    }

                    if(processPid == 0)
                    {
                        char processIndex[12];
                        sprintf(processIndex, "%d", i);
                        execl("./process", "process", processIndex, NULL);
                        fprintf(stderr, "%s: Error: execl failed.", argv[0]);
                    }
                    totalProcesses++;
                    activeChildren++;
                }
            }
        }

        int i;
        int j;
        for(i = 0; i < 18; i++)
        {
            waitpid(-1, NULL, WNOHANG);
            if(sharedPcb[i].terminated == 1)
            {
                sharedPcb[i].terminated = 0;
                bitmap[0] &= ~(1 << i);
                for(j = 0; j < 20; j++)
                {
                    sharedResource[j].availableResource = sharedResource[j].availableResource + sharedPcb[i].allocated[j];
                    sharedPcb[i].allocated[j] = 0;
                }
                sharedPcb[i].deadlocked = 0;
                sharedPcb[i].inQueue = 0;
                activeChildren--;
                waitpid(-1, NULL, WNOHANG);
                waitpid(-1, NULL, WNOHANG);
                waitpid(-1, NULL, WNOHANG);
                waitpid(-1, NULL, WNOHANG);
            }
        }

        queueRequests(resourceQueue);
        grantRequests(resourceQueue);
        isDeadlocked = deadlockCheck();
        if(isDeadlocked)
        {
            deadlockRecovery();
            isDeadlocked = 0;
        }
    }

    free(resourceQueue);
    shmdt(sharedClock);
    shmdt(sharedPcb);
    shmdt(sharedResource);
    shmctl(pcbShmid, IPC_RMID, NULL);
    shmctl(resourceShmid, IPC_RMID, NULL);
    shmctl(clockShmid, IPC_RMID, NULL);
    semctl(clockSem, 0, IPC_RMID);

    return 0;
}

void createQueue(struct queue* que)
{
    que->first = NULL;
    que->last = NULL;
}

void enqueue(struct queue* que, int pid)
{
    struct node* temp;
    temp = malloc(sizeof(struct node));
    if(temp == NULL)
    {
        perror("");
        shmctl(pcbShmid, IPC_RMID, NULL);
        shmctl(resourceShmid, IPC_RMID, NULL);
        shmctl(clockShmid, IPC_RMID, NULL);
        semctl(clockSem, 0, IPC_RMID);
        exit(EXIT_FAILURE);
    }
    temp->pid = pid;
    temp->next = NULL;

    if(que->first == NULL)
    {
        que->first = temp;
        que->last = temp;
    }

    else
    {
        que->last->next = temp;
        que->last = temp;
    }
}

int dequeue(struct queue* que)
{
    struct node* temp;
    int pid = que->first->pid;
    temp = que->first;
    que->first = que->first->next;
    free(temp);
    return(pid);
}

int isEmpty(struct queue* que)
{
    if(que->first == NULL)
    {
        return 1;
    }
    else
    {
        return 0;
    }
}

void queueRequests(struct queue* que)
{
    int i;
    for (i = 0; i < 18; i++)
    {
        if(sharedPcb[i].inQueue == 0)
        {
            if(sharedPcb[i].typeResourceRequested != -1)
            {
                enqueue(que, sharedPcb[i].simPid);
                sharedPcb[i].inQueue = 1;
                numberInQueue++;
            }

        }
    }
}

void grantRequests(struct queue* que)
{
    int simPid;
    int neededResource;
    int numberLoop = numberInQueue;
    int i;
    for(i = 0; i < numberLoop; i++)
    {
        simPid = dequeue(que);
        if(sharedPcb[simPid].inQueue == 0)
        {
            continue;
        }
        neededResource = sharedPcb[simPid].typeResourceRequested;
        if(sharedResource[neededResource].shareable == 1 && sharedPcb[simPid].inQueue == 1)
        {
            sharedPcb[simPid].allocated[neededResource] = sharedPcb[simPid].allocated[neededResource] + sharedPcb[simPid].numberOfResourceRequested;
            sharedPcb[simPid].numberOfResourceRequested = 0;
            sharedPcb[simPid].inQueue = 0;
            sharedPcb[simPid].typeResourceRequested = -1;
            requestsGranted++;
            numberInQueue--;
            continue;
        }
        if(sharedPcb[simPid].numberOfResourceRequested <= sharedResource[neededResource].availableResource && sharedPcb[simPid].inQueue == 1)
        {
            sharedResource[neededResource].availableResource = sharedResource[neededResource].availableResource - sharedPcb[simPid].numberOfResourceRequested;
            sharedResource[neededResource].allocatedResource = sharedResource[neededResource].allocatedResource + sharedPcb[simPid].numberOfResourceRequested;
            sharedPcb[simPid].allocated[neededResource] = sharedPcb[simPid].allocated[neededResource] + sharedPcb[simPid].numberOfResourceRequested;
            sharedPcb[simPid].inQueue = 0;
            sharedPcb[simPid].typeResourceRequested = -1;
            sharedPcb[simPid].numberOfResourceRequested = 0;
            requestsGranted++;
            numberInQueue--;
        }
        else if(sharedPcb[simPid].inQueue == 1)
        {
            enqueue(que, simPid);
        }
    }
}

int deadlockCheck()
{
    int work[20];
    int finish[18];
    int i;
    int finishChanged = 1;
    int isDeadlocked = 0;

    for(i = 0; i < 20; i++)
    {
        work[i] = sharedResource[i].availableResource;
    }

    for(i = 0; i < 18; i++)
    {
        if(sharedPcb[i].inQueue == 0)
        {
            finish[i] = 1;
            int j;
            for(j = 0; j < 20; j++)
            {
                work[j] = work[j] + sharedPcb[i].allocated[j];
            }
        }
        else
        {
            finish[i] = 0;
        }
    }
    while(finishChanged)
    {
        finishChanged = 0;
        for(i = 0; i < 18; i++)
        {
            if(finish[i] == 0 && sharedPcb[i].numberOfResourceRequested <= work[sharedPcb[i].typeResourceRequested])
            {
                int j;
                for(j = 0; j < 20; j++)
                {
                    work[j] = work[j] + sharedPcb[i].allocated[j];
                }
                finish[i] = 1;
                sharedPcb[i].deadlocked = 0;
                finishChanged = 1;
            }
        }
    }

    for(i = 0; i < 18; i++)
    {
        if(finish[i] == 0)
        {
            sharedPcb[i].deadlocked = 1;
            isDeadlocked = 1;
        }
    }

    return isDeadlocked;
}

void deadlockRecovery()
{
    int isDeadlocked = 1;
    int i;
    int j;
    while(isDeadlocked)
    {
        for(i = 0; i < 18; i++)
        {
            if(sharedPcb[i].deadlocked == 1)
            {
                kill(sharedPcb[i].actualPid, SIGKILL);
                for(j = 0; j < 20; j++)
                {
                    sharedResource[j].availableResource = sharedResource[j].availableResource + sharedPcb[i].allocated[j];
                    sharedPcb[i].allocated[j] = 0;
                }
                sharedPcb[i].deadlocked = 0;
                sharedPcb[i].inQueue = 0;
                sharedPcb[i].typeResourceRequested = -1;
                sharedPcb[i].numberOfResourceRequested = 0;
                numberInQueue--;
                activeChildren--;
                bitmap[0] &= ~(1 << i);
                waitpid(-1, NULL, WNOHANG);
                waitpid(-1, NULL, WNOHANG);
                break;
            }
        }
        isDeadlocked = deadlockCheck();
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

