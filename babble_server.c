#include "babble_server.h"

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
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

#define BUFFER_SIZE 16

sem_t full_count, empty_count, l_lock;
command_t **cmd_buffer;
int buffer_count;


int buffer init(){
    buffer_count = 0;
    cmd_buffer=malloc(BUFFER_SIZE* sizeof(*command_t));
    if (sem_init(&full_count, 0, 0) != 0) {
        perror("sem_init full_count");
        exit(-1);
    }
    if (sem_init(&empty_count, 0, BUFFER_SIZE) != 0) {
        perror("sem_init empty_count");
        exit(-1);
    }
    if (sem_init(&l_lock, 0, 1) != 0) {
        perror("sem_init l_lock");
        exit(-1);
    }
}


int add_to_buffer(command_t *cmd){
    sem_wait(empty_count);
    sem_wait(l_lock);
    cmd_buffer[buffer_count]=cmd;
    buffer_count++;
    sem_post(l_lock);
    sem_post(full_count);
}

*command_t write_to_buffer(char *str){
    sem_wait(empty_count);
    sem_wait(l_lock);
    *command_t res = cmd_buffer[buffer_count];
    buffer_count--;
    sem_post(l_lock);
    sem_post(full_count);
}




static void display_help(char *exec) {
  printf("Usage: %s -p port_number\n", exec);
}

static int parse_command(char *str, command_t *cmd) {
  char *name = NULL;

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
        return -1;
      }
      break;
    case PUBLISH:
      if (str_to_payload(str, cmd->msg, BABBLE_SIZE)) {
        name = get_name_from_key(cmd->key);
        fprintf(stderr, "Warning from [%s]-- invalid PUBLISH -> %s\n", name,
                str);
        free(name);
        return -1;
      }
      break;
    case FOLLOW:
      if (str_to_payload(str, cmd->msg, BABBLE_ID_SIZE)) {
        name = get_name_from_key(cmd->key);
        fprintf(stderr, "Warning from [%s]-- invalid FOLLOW -> %s\n", name,
                str);
        free(name);
        return -1;
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
      return -1;
  }

  return 0;
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

void *exec_routine(void *args) { return NULL; }

void *comm_routine(void *args) {

    //Shouldn't this be inside the main thread?
    //registering client plus sending back the login acknowledgement
  int sockfd = *((int *)args);
  char client_name[BABBLE_ID_SIZE + 1];
  char *recv_buff = NULL;
  int recv_size = 0;
  command_t *cmd;
  answer_t *answer = NULL;
  unsigned long client_key = 0;

  printf("socket: %d\n", sockfd);
  memset(client_name, 0, BABBLE_ID_SIZE + 1);
  if ((recv_size = network_recv(sockfd, (void **)&recv_buff)) < 0) {
    fprintf(stderr, "Error -- recv from client\n");
    close(sockfd);
  }

  cmd = new_command(0);

  if (parse_command(recv_buff, cmd) == -1 || cmd->cid != LOGIN) {
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
    if (parse_command(recv_buff, cmd) == -1) {
      fprintf(stderr, "Warning: unable to parse message from client %s\n",
              client_name);
      notify_parse_error(cmd, recv_buff, &answer);
      send_answer_to_client(answer);
      free_answer(answer);
      free(cmd);
    } else {
      if (process_command(cmd, &answer) == -1) {
        fprintf(stderr, "Warning: unable to process command from client %lu\n",
                cmd->key);
      }
      free(cmd);

      if (send_answer_to_client(answer) == -1) {
        fprintf(stderr, "Warning: unable to answer command from client %lu\n",
                answer->key);
      }
      free_answer(answer);
    }
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
  close(sockfd);
  return NULL;
}

int main(int argc, char *argv[]) {
  int sockfd, newsockfd[BABBLE_ANSWER_THREADS];
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

  server_data_init();

  if ((sockfd = server_connection_init(portno)) == -1) {
    return -1;
  }

  for (i = 0; i < BABBLE_EXECUTOR_THREADS; i++) {
    pthread_create(&execs[logged_in], NULL, exec_routine, NULL);
  }

  printf("Babble server bound to port %d\n", portno);

  /* main server loop */
  while (1) {
    if ((newsockfd[logged_in] = server_connection_accept(sockfd)) == -1) {
      return -1;
    }
    pthread_create(&clients[logged_in], NULL, comm_routine,
                   &newsockfd[logged_in]);
    logged_in++;
  }
  return 0;
}
