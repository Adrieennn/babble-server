#ifndef __BABBLE_TYPES_H__
#define __BABBLE_TYPES_H__

#include <time.h>

#include "babble_config.h"
#include "babble_publication_set.h"

typedef enum {
  LOGIN = 0,
  PUBLISH,
  FOLLOW,
  TIMELINE,
  FOLLOW_COUNT,
  RDV,
  UNREGISTER
} command_id;

typedef struct command {
  command_id cid;
  int sock; /* only needed by the LOGIN command, other commands
             * will use the key */
  unsigned long key;
  char msg[BABBLE_SIZE];
  char recv_buf[BABBLE_BUFFER_SIZE];
  int answer_expected; /* answer sent only if set */
  int return_status; /* return status of parse_command */
} command_t;

typedef struct client_bundle {
  unsigned long key;                /* hash of the name */
  char client_name[BABBLE_ID_SIZE]; /* name as provided by the
                                     * client */
  int sock;                         /* socket to communicate with this client */

  publication_set_t *pub_set; /* set of messages published by the
                               * client */

  struct client_bundle *followed[MAX_CLIENT]; /* key of the followedK
                                               * clients */
  int nb_followed;
  uint64_t last_timeline; /* stored to display only *new* messages */
  int nb_followers;
  unsigned int disconnected; /* set to 1 when client has disconnected */

} client_bundle_t;

#endif

