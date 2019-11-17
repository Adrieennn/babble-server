#include "babble_registration.h"

#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

sem_t table_lock;
client_bundle_t* registration_table[MAX_CLIENT];
int nb_registered_clients;

void registration_init(void) {
  nb_registered_clients = 0;

  memset(registration_table, 0, MAX_CLIENT * sizeof(client_bundle_t*));

  if (sem_init(&table_lock, 0, 1) != 0) {
    perror("sem_init");
    exit(-1);
  }
}

client_bundle_t* registration_lookup(unsigned long key) {
  int i = 0;
  client_bundle_t* c = NULL;

  sem_wait(&table_lock);
  for (i = 0; i < nb_registered_clients; i++) {
    if (registration_table[i]->key == key) {
      c = registration_table[i];
      break;
    }
  }
  
  sem_post(&table_lock);
  return c;
}

int registration_insert(client_bundle_t* cl) {
  sem_wait(&table_lock);
  if (nb_registered_clients == MAX_CLIENT) {
    fprintf(stderr, "ERROR: MAX NUMBER OF CLIENTS REACHED\n");
    sem_post(&table_lock);
    return -1;
  }

  /* lookup to find if key already exists*/
  int i = 0;

  for (i = 0; i < nb_registered_clients; i++) {
    if (registration_table[i]->key == cl->key) {
      break;
    }
  }

  if (i != nb_registered_clients) {
    fprintf(stderr, "Error -- id % ld already in use\n", cl->key);
    sem_post(&table_lock);
    return -1;
  }

  /* insert cl */
  registration_table[nb_registered_clients] = cl;
  nb_registered_clients++;

  sem_post(&table_lock);
  return 0;
}

client_bundle_t* registration_remove(unsigned long key) {
  int i = 0;

  sem_wait(&table_lock);
  for (i = 0; i < nb_registered_clients; i++) {
    if (registration_table[i]->key == key) {
      break;
    }
  }

  if (i == nb_registered_clients) {
    fprintf(stderr, "Error -- no client found\n");
    sem_post(&table_lock);
    return NULL;
  }

  client_bundle_t* cl = registration_table[i];

  nb_registered_clients--;
  registration_table[i] = registration_table[nb_registered_clients];

  sem_post(&table_lock);
  return cl;
}
