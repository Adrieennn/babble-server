#include "babble_server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "babble_communication.h"
#include "babble_server_answer.h"
#include "babble_types.h"
#include "babble_utils.h"

/* command buffer variables */
sem_t full_count, empty_count, cmd_lock;
command_t **cmd_buffer;
int buffer_in, buffer_out;

/* preventing DoS */
int shared_fd;
sem_t fd_to_pass, fd_passed;
sem_t process_lock;

void buffer_init() {
  buffer_in = 0;
  buffer_out = 0;
  cmd_buffer = malloc(BABBLE_PRODCONS_SIZE * sizeof(command_t *));

  if (sem_init(&full_count, 0, 0) != 0) {
    perror("sem_init full_count");
    exit(-1);
  }

  if (sem_init(&empty_count, 0, BABBLE_PRODCONS_SIZE) != 0) {
    perror("sem_init empty_count");
    exit(-1);
  }

  if (sem_init(&cmd_lock, 0, 1) != 0) {
    perror("sem_init l_lock");
    exit(-1);
  }
}

void write_to_buffer(command_t *cmd) {
  sem_wait(&empty_count);
  sem_wait(&cmd_lock);
  cmd_buffer[buffer_in] = cmd;
  buffer_in = (buffer_in + 1) % BABBLE_PRODCONS_SIZE;
  sem_post(&cmd_lock);
  sem_post(&full_count);
}

command_t *read_from_buffer() {
  sem_wait(&full_count);
  sem_wait(&cmd_lock);
  command_t *res = cmd_buffer[buffer_out];
  buffer_out = (buffer_out + 1) % BABBLE_PRODCONS_SIZE;
  sem_post(&cmd_lock);
  sem_post(&empty_count);
  return res;
}

static void display_help(char *exec) {
  printf("Usage: %s -p port_number\n", exec);
}

static void parse_command(char *str, command_t *cmd) {
  char *name = NULL;

  strncpy(cmd->recv_buf, str, BABBLE_BUFFER_SIZE);
  /* start by cleaning the input */
  str_clean(str);

  /* get command id */
  cmd->cid = str_to_command(str, &cmd->answer_expected);

  switch (cmd->cid) {
    case LOGIN:
      if (str_to_payload(str, cmd->msg, BABBLE_ID_SIZE)) {
        name = get_name_from_key(cmd->key);
        fprintf(stderr, "Error from [%s]-- invalid LOGIN -> %s\n", name, str);
        free(name);
        cmd->return_status = -1;
        return;
      }
      break;
    case PUBLISH:
      if (str_to_payload(str, cmd->msg, BABBLE_SIZE)) {
        name = get_name_from_key(cmd->key);
        fprintf(stderr, "Warning from [%s]-- invalid PUBLISH -> %s\n", name,
                str);
        free(name);
        cmd->return_status = -1;
        return;
      }
      break;
    case FOLLOW:
      if (str_to_payload(str, cmd->msg, BABBLE_ID_SIZE)) {
        name = get_name_from_key(cmd->key);
        fprintf(stderr, "Warning from [%s]-- invalid FOLLOW -> %s\n", name,
                str);
        free(name);
        cmd->return_status = -1;
        return;
      }
      break;
    case TIMELINE:
      cmd->msg[0] = '\0';
      break;
    case FOLLOW_COUNT:
      cmd->msg[0] = '\0';
      break;
    case RDV:
      cmd->msg[0] = '\0';
      break;
    default:
      name = get_name_from_key(cmd->key);
      fprintf(stderr, "Error from [%s]-- invalid client command -> %s\n", name,
              str);
      free(name);
      cmd->return_status = -1;
      return;
  }

  cmd->return_status = 0;
  return;
}

/* processes the command and eventually generates an answer */
static int process_command(command_t *cmd, answer_t **answer) {
  int res = 0;

  switch (cmd->cid) {
    case LOGIN:
      res = run_login_command(cmd, answer);
      break;
    case PUBLISH:
      res = run_publish_command(cmd, answer);
      break;
    case FOLLOW:
      res = run_follow_command(cmd, answer);
      break;
    case TIMELINE:
      res = run_timeline_command(cmd, answer);
      break;
    case FOLLOW_COUNT:
      res = run_fcount_command(cmd, answer);
      break;
    case RDV:
      res = run_rdv_command(cmd, answer);
      break;
    default:
      fprintf(stderr, "Error -- Unknown command id\n");
      return -1;
  }

  if (res) {
    fprintf(stderr, "Error -- Failed to run command ");
    display_command(cmd, stderr);
  }

  return res;
}

void *exec_routine(void *args) {
  /* execution threads */
  command_t *cmd;
  answer_t *answer = NULL;
  char client_name[BABBLE_ID_SIZE + 1];
  int ps_status;

  while (1) {
    /* get the command from the command buffer*/
    cmd = read_from_buffer();
    if (cmd->return_status == -1) {
      strncpy(client_name, cmd->msg, BABBLE_ID_SIZE);
      fprintf(stderr, "Warning: unable to parse message from client %s\n",
              client_name);
      notify_parse_error(cmd, cmd->recv_buf, &answer);
      send_answer_to_client(answer);
      free_answer(answer);
      free(cmd);
    } else {
      /* process it */
      sem_wait(&cmd_lock);
      ps_status = process_command(cmd, &answer);
      sem_post(&cmd_lock);
      if (ps_status == -1) {
        fprintf(stderr, "Warning: unable to process command from client %lu\n",
                cmd->key);
      }
      free(cmd);

      /* send answer*/
      if (send_answer_to_client(answer) == -1) {
        fprintf(stderr, "Warning: unable to answer command from client %lu\n",
                answer->key);
      }
      free_answer(answer);
    }
  }
  return NULL;
}

void *comm_routine(void *args) {
  (void)args;

  while (1) {
    sem_wait(&fd_to_pass);
    int sockfd = shared_fd;
    sem_post(&fd_passed);

    char client_name[BABBLE_ID_SIZE + 1];
    char *recv_buff = NULL;
    int recv_size = 0;
    command_t *cmd;
    answer_t *answer = NULL;
    unsigned long client_key = 0;

    memset(client_name, 0, BABBLE_ID_SIZE + 1);
    if ((recv_size = network_recv(sockfd, (void **)&recv_buff)) < 0) {
      fprintf(stderr, "Error -- recv from client\n");
      close(sockfd);
    }

    cmd = new_command(0);

    parse_command(recv_buff, cmd);
    if (cmd->return_status == -1 || cmd->cid != LOGIN) {
      fprintf(stderr, "Error -- in LOGIN message\n");
      close(sockfd);
      free(cmd);
    }

    /* before processing the command, we should register the
     * socket associated with the new client; this is to be done only
     * for the LOGIN command */
    cmd->sock = sockfd;

    if (process_command(cmd, &answer) == -1) {
      fprintf(stderr, "Error -- in LOGIN\n");
      close(sockfd);
      free(cmd);
    }

    /* notify client of registration */
    if (send_answer_to_client(answer) == -1) {
      fprintf(stderr, "Error -- in LOGIN ack\n");
      close(sockfd);
      free(cmd);
      free_answer(answer);
    } else {
      free_answer(answer);
    }

    /* let's store the key locally */
    client_key = cmd->key;

    strncpy(client_name, cmd->msg, BABBLE_ID_SIZE);
    free(recv_buff);
    free(cmd);

    /* looping on client commands */
    while ((recv_size = network_recv(sockfd, (void **)&recv_buff)) > 0) {
      cmd = new_command(client_key);
      parse_command(recv_buff, cmd);

      write_to_buffer(cmd);
      free(recv_buff);
    }

    /* UNREGISTERING client */
    if (client_name[0] != 0) {
      cmd = new_command(client_key);
      cmd->cid = UNREGISTER;

      if (unregisted_client(cmd)) {
        fprintf(stderr, "Warning -- failed to unregister client %s\n",
                client_name);
      }
      free(cmd);
    }
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  int sockfd;
  int portno = BABBLE_PORT;

  int opt, i;
  int nb_args = 1;

  pthread_t clients[BABBLE_ANSWER_THREADS];
  pthread_t execs[BABBLE_EXECUTOR_THREADS];
  unsigned int logged_in = 0;

  while ((opt = getopt(argc, argv, "+hp:")) != -1) {
    switch (opt) {
      case 'p':
        portno = atoi(optarg);
        nb_args += 2;
        break;
      case 'h':
      case '?':
      default:
        display_help(argv[0]);
        return -1;
    }
  }

  if (nb_args != argc) {
    display_help(argv[0]);
    return -1;
  }

  /* command buffer initialization */
  buffer_init();
  server_data_init();

  if ((sockfd = server_connection_init(portno)) == -1) {
    return -1;
  }

  /* initialized at locked state */
  if (sem_init(&fd_to_pass, 0, 0) != 0) {
    perror("sem_init fd_lock");
    exit(-1);
  }

  /* initialized at locked state */
  if (sem_init(&fd_passed, 0, 0) != 0) {
    perror("sem_init fd_lock");
    exit(-1);
  }

  /* initialized as a lock */
  if (sem_init(&cmd_lock, 0, 1) != 0) {
    perror("sem_init cmd_lock");
    exit(-1);
  }

  for (i = 0; i < BABBLE_EXECUTOR_THREADS; i++) {
    pthread_create(&execs[logged_in], NULL, exec_routine, NULL);
  }

  for (i = 0; i < BABBLE_ANSWER_THREADS; i++) {
    pthread_create(&clients[logged_in], NULL, comm_routine, NULL);
  }

  printf("Babble server bound to port %d\n", portno);

  /* main server loop */
  while (1) {
    if ((shared_fd = server_connection_accept(sockfd)) == -1) {
      return -1;
    }
    /* passing the socket fd to a comm thread */
    sem_post(&fd_to_pass);
    /* waiting for comm thread to confirm reception */
    sem_wait(&fd_passed);
  }
  return 0;
}
