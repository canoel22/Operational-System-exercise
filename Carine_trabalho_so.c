#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/msg.h>

#define NUM_FILHOS 4
#define MUTEX_NAME "/mutex_exclusao"

struct mensagem {
    long tipo;
    pid_t pid;
};

typedef struct {
    int valor_copiado;
    pid_t pid;
} DadosThread;

void* imprimir_info(void* arg) {
    DadosThread* dados = (DadosThread*) arg;
    printf("Thread no processo (PID %d): valor copiado da memória compartilhada = %d\n",
           dados->pid, dados->valor_copiado);
    pthread_exit(NULL);
}

int main() {
    int id_memoria, id_fila;
    int *variavel_compartilhada;
    key_t chave_mem = ftok("shmfile", 65);
    key_t chave_msg = ftok("msgfile", 75);  // gere com: touch msgfile
    pid_t filhos[NUM_FILHOS];
    char nomes_sem[NUM_FILHOS][32];

    if (chave_mem == -1 || chave_msg == -1) {
        perror("Erro ao gerar chave");
        exit(1);
    }

    // Criação da memória compartilhada
    id_memoria = shmget(chave_mem, sizeof(int), 0666 | IPC_CREAT);
    if (id_memoria < 0) {
        perror("Erro ao criar memória compartilhada");
        exit(1);
    }

    variavel_compartilhada = (int *) shmat(id_memoria, NULL, 0);
    if (variavel_compartilhada == (int *) -1) {
        perror("Erro ao anexar memória compartilhada");
        exit(1);
    }

    *variavel_compartilhada = 1;
    printf("Pai: variável compartilhada inicializada com valor = %d\n", *variavel_compartilhada);

    // Criação do semáforo de exclusão mútua
    sem_unlink(MUTEX_NAME);
    sem_t *mutex = sem_open(MUTEX_NAME, O_CREAT | O_EXCL, 0600, 1);
    if (mutex == SEM_FAILED) {
        perror("Erro ao criar semáforo de mutex");
        exit(1);
    }

    // Criação da fila de mensagens
    id_fila = msgget(chave_msg, IPC_CREAT | 0666);
    if (id_fila < 0) {
        perror("Erro ao criar fila de mensagens");
        exit(1);
    }

    // Criação dos semáforos individuais
    sem_t *semaforos[NUM_FILHOS];
    for (int i = 0; i < NUM_FILHOS; i++) {
        snprintf(nomes_sem[i], sizeof(nomes_sem[i]), "/sem_filho_%d", i);
        sem_unlink(nomes_sem[i]);
        semaforos[i] = sem_open(nomes_sem[i], O_CREAT | O_EXCL, 0600, 0);
        if (semaforos[i] == SEM_FAILED) {
            perror("Erro ao criar semáforo individual");
            exit(1);
        }
    }

    // Criação dos filhos
    for (int i = 0; i < NUM_FILHOS; i++) {
        filhos[i] = fork();

        if (filhos[i] < 0) {
            perror("Erro ao criar filho");
            exit(1);
        } else if (filhos[i] == 0) {
            // Processo filho
            sem_t *sem_filho = sem_open(nomes_sem[i], 0);
            sem_t *mutex_filho = sem_open(MUTEX_NAME, 0);
            if (sem_filho == SEM_FAILED || mutex_filho == SEM_FAILED) {
                perror("Filho: erro ao abrir semáforos");
                exit(1);
            }

            sem_wait(sem_filho);

            srand(getpid());
            int espera_inicial = 200000 + rand() % 800001;
            usleep(espera_inicial);

            sem_wait(mutex_filho);
            int *var = (int *) shmat(id_memoria, NULL, 0);
            int copia_local = *var;
            (*var)--;  // decrementa valor
            shmdt(var);
            sem_post(mutex_filho);

            int espera_final = 500000 + rand() % 500001;
            usleep(espera_final);

            // Cria thread
            pthread_t thread;
            DadosThread dados = { copia_local, getpid() };
            pthread_create(&thread, NULL, imprimir_info, &dados);
            pthread_join(thread, NULL);

            // Mensagem final do processo filho
            printf("Processo filho (PID %d): finalizando\n", getpid());

            // Envia mensagem ao pai
            struct mensagem msg;
            msg.tipo = 1;
            msg.pid = getpid();
            msgsnd(id_fila, &msg, sizeof(pid_t), 0);

            sem_close(mutex_filho);
            sem_close(sem_filho);
            exit(0);
        }
    }

    // Pai dorme e libera os filhos
    sleep(2);
    printf("\nPai: liberando todos os filhos...\n");
    for (int i = 0; i < NUM_FILHOS; i++) {
        sem_post(semaforos[i]);
    }

    // Espera todos os filhos
    for (int i = 0; i < NUM_FILHOS; i++) {
        waitpid(filhos[i], NULL, 0);
        sem_close(semaforos[i]);
        sem_unlink(nomes_sem[i]);
    }

    // Recebe mensagens dos filhos
    printf("\nPai: recebendo mensagens de finalização dos filhos:\n");
    for (int i = 0; i < NUM_FILHOS; i++) {
        struct mensagem msg;
        msgrcv(id_fila, &msg, sizeof(pid_t), 1, 0);
    }

    // Mensagem final usando acesso direto à memória compartilhada
    printf("\nProcesso pai de pid %d informa que todos os filhos foram finalizados e o valor atual da memória compartilhada é %d.\n", getpid(), *variavel_compartilhada);

    // Limpeza final
    sem_close(mutex);
    sem_unlink(MUTEX_NAME);
    shmdt(variavel_compartilhada);
    shmctl(id_memoria, IPC_RMID, NULL);
    msgctl(id_fila, IPC_RMID, NULL);

    return 0;

}

