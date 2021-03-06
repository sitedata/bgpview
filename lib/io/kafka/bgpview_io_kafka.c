/*
 * Copyright (C) 2014 The Regents of the University of California.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "bgpview_io_kafka_int.h"
#include "bgpview.h"
#include "config.h"
#include "utils.h"
#include "parse_cmd.h"
#include <assert.h>
#include <errno.h>
#include <librdkafka/rdkafka.h>
#include <string.h>
#include <unistd.h>
#ifdef HAVE_TIME_H
#include <time.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif

/* ========== PRIVATE FUNCTIONS ========== */

static void kafka_error_callback(rd_kafka_t *rk, int err, const char *reason,
                                 void *opaque)
{
  bgpview_io_kafka_t *client = (bgpview_io_kafka_t *)opaque;

  switch (err) {
  // fatal errors:
  case RD_KAFKA_RESP_ERR__BAD_COMPRESSION:
  case RD_KAFKA_RESP_ERR__RESOLVE:
    client->fatal_error = 1;
  // fall through

  // recoverable? errors:
  case RD_KAFKA_RESP_ERR__DESTROY:
  case RD_KAFKA_RESP_ERR__FAIL:
  case RD_KAFKA_RESP_ERR__TRANSPORT:
  case RD_KAFKA_RESP_ERR__ALL_BROKERS_DOWN:
    client->connected = 0;
    break;
  }

  fprintf(stderr, "ERROR: %s (%d): %s\n", rd_kafka_err2str(err), err, reason);

  // TODO: handle other errors
}

static void free_gc_topics(gc_topics_t *gct)
{
  fprintf(stderr, "INFO: Destroying state for %s\n", gct->pfxs.name);
#ifdef WITH_THREADS
  /* clean up the worker thread */
  gct->shutdown = 1;
  pthread_cond_signal(&gct->job_state_cond);
  pthread_join(gct->worker, NULL);
  pthread_mutex_destroy(&gct->mutex);
  pthread_cond_destroy(&gct->job_state_cond);
  pthread_cond_destroy(&gct->worker_state_cond);
#endif

  free(gct->idmap.map);
  gct->idmap.map = NULL;
  gct->idmap.alloc_cnt = 0;

  if (gct->peers.rkt != NULL) {
    rd_kafka_topic_destroy(gct->peers.rkt);
    gct->peers.rkt = NULL;
  }
  if (gct->pfxs.rkt != NULL) {
    rd_kafka_topic_destroy(gct->pfxs.rkt);
    gct->pfxs.rkt = NULL;
  }

  free(gct);
}

static int kafka_topic_connect(bgpview_io_kafka_t *client)
{
  bgpview_io_kafka_topic_id_t id;

  fprintf(stderr, "INFO: Checking topic connections...\n");

  for (id = 0; id < BGPVIEW_IO_KAFKA_TOPIC_ID_CNT; id++) {

    // producer gets: pfxs, peers, meta, members
    // direct consumer gets: pfxs, peers, meta
    // global consumer gets: globalmeta
    if ((client->mode == BGPVIEW_IO_KAFKA_MODE_PRODUCER &&
         id == BGPVIEW_IO_KAFKA_TOPIC_ID_GLOBALMETA) ||
        (client->mode == BGPVIEW_IO_KAFKA_MODE_DIRECT_CONSUMER &&
         (id == BGPVIEW_IO_KAFKA_TOPIC_ID_MEMBERS ||
          id == BGPVIEW_IO_KAFKA_TOPIC_ID_GLOBALMETA)) ||
        (client->mode == BGPVIEW_IO_KAFKA_MODE_GLOBAL_CONSUMER &&
         id != BGPVIEW_IO_KAFKA_TOPIC_ID_GLOBALMETA)) {
      continue;
    }

    assert(client->namespace != NULL);

    if (bgpview_io_kafka_single_topic_connect(client, client->identity, id,
                                              TOPIC(id)) != 0) {
      return -1;
    }
  }

  return 0;
}

static void usage(void)
{
  fprintf(
    stderr,
    "Kafka Consumer Options:\n"
    "       -i <identity>         Consume directly from the given producer\n"
    "                             (rather than a global view from all "
    "producers)\n"
    "       -k <kafka-brokers>    List of Kafka brokers (default: %s)\n"
    "       -n <namespace>        Kafka topic namespace to use (default: "
    "%s)\n"
    "       -c <channel>          Global metadata channel to use (default: "
    "unused)\n",
    BGPVIEW_IO_KAFKA_BROKER_URI_DEFAULT, BGPVIEW_IO_KAFKA_NAMESPACE_DEFAULT);
}

static int parse_args(bgpview_io_kafka_t *client, int argc, char **argv)
{
  int opt;
  assert(argc > 0 && argv != NULL);
  /* NB: remember to reset optind to 1 before using getopt! */
  optind = 1;

  /* remember the argv strings DO NOT belong to us */
  while ((opt = getopt(argc, argv, ":c:i:k:n:?")) >= 0) {
    switch (opt) {
    case 'c':
      client->channel = strdup(optarg);
      break;

    case 'i':
      client->identity = strdup(optarg);
      break;

    case 'k':
      if (bgpview_io_kafka_set_broker_addresses(client, optarg) != 0) {
        return -1;
      }
      break;

    case 'n':
      if (bgpview_io_kafka_set_namespace(client, optarg) != 0) {
        return -1;
      }
      break;

    case '?':
    case ':':
    default:
      usage();
      return -1;
    }
  }
  return 0;
}

/* ========== PROTECTED FUNCTIONS ========== */

int bgpview_io_kafka_common_config(bgpview_io_kafka_t *client,
                                   rd_kafka_conf_t *conf)
{
  char errstr[512];

  // Set the opaque pointer that will be passed to callbacks
  rd_kafka_conf_set_opaque(conf, client);

  // Set our error handler
  rd_kafka_conf_set_error_cb(conf, kafka_error_callback);

  // Disable logging of connection close/idle timeouts caused by Kafka 0.9.x
  //   See https://github.com/edenhill/librdkafka/issues/437 for more details.
  // TODO: change this when librdkafka has better handling of idle disconnects
  if (rd_kafka_conf_set(conf, "log.connection.close", "false", errstr,
                        sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    fprintf(stderr, "ERROR: %s\n", errstr);
    goto err;
  }

  if (rd_kafka_conf_set(conf, "api.version.request", "true", errstr,
                        sizeof(errstr)) != RD_KAFKA_CONF_OK) {
    fprintf(stderr, "ERROR: %s\n", errstr);
    goto err;
  }

  return 0;

err:
  return -1;
}

int bgpview_io_kafka_single_topic_connect(bgpview_io_kafka_t *client,
                                          char *identity,
                                          bgpview_io_kafka_topic_id_t id,
                                          bgpview_io_kafka_topic_t *topic)
{
  static const char *names[] = {
    "pfxs", "peers", "meta", "members", "globalmeta",
  };

  typedef int(topic_connect_func_t)(bgpview_io_kafka_t * client,
                                    rd_kafka_topic_t * *rkt, char *topic);

  topic_connect_func_t *topic_connect_funcs[] = {
    // BGPVIEW_IO_KAFKA_MODE_DIRECT_CONSUMER
    bgpview_io_kafka_consumer_topic_connect,

    // BGPVIEW_IO_KAFKA_MODE_GLOBAL_CONSUMER
    bgpview_io_kafka_consumer_topic_connect,

    // AUTO CONSUMER
    NULL,

    // BGPVIEW_IO_KAFKA_MODE_PRODUCER
    bgpview_io_kafka_producer_topic_connect,
  };

  // build the name
  if (id == BGPVIEW_IO_KAFKA_TOPIC_ID_MEMBERS ||
      id == BGPVIEW_IO_KAFKA_TOPIC_ID_META ||
      (id == BGPVIEW_IO_KAFKA_TOPIC_ID_GLOBALMETA && client->channel == NULL)) {
    // format: <namespace>.<name>
    if (snprintf(topic->name, IDENTITY_MAX_LEN, "%s.%s", client->namespace,
                 names[id]) >= IDENTITY_MAX_LEN) {
      return -1;
    }
  } else if (id == BGPVIEW_IO_KAFKA_TOPIC_ID_GLOBALMETA) {
    // format: <namespace>.<name>.<channel>
    assert(client->channel != NULL);
    if (snprintf(topic->name, IDENTITY_MAX_LEN, "%s.%s.%s", client->namespace,
                 names[id], client->channel) >= IDENTITY_MAX_LEN) {
      return -1;
    }
  } else {
    // format: <namespace>.<identity>.<name>
    assert(identity != NULL);
    if (snprintf(topic->name, IDENTITY_MAX_LEN, "%s.%s.%s", client->namespace,
                 identity, names[id]) >= IDENTITY_MAX_LEN) {
      return -1;
    }
  }

  // connect to kafka
  if (topic->rkt == NULL) {
    /*fprintf(stderr, "INFO: Connecting to %s (%d)\n", topic->name, id);*/
    if (topic_connect_funcs[client->mode](client, &topic->rkt, topic->name) !=
        0) {
      return -1;
    }
  }

  return 0;
}

/* ========== PUBLIC FUNCTIONS ========== */

bgpview_io_kafka_t *bgpview_io_kafka_init(bgpview_io_kafka_mode_t mode,
                                          const char *opts)
{
#define MAXOPTS 1024
  char *local_args = NULL;
  char *process_argv[MAXOPTS];
  int len;
  int process_argc = 0;

  bgpview_io_kafka_t *client;
  if ((client = malloc_zero(sizeof(bgpview_io_kafka_t))) == NULL) {
    return NULL;
  }

  client->mode = mode;

  /* set defaults */
  if ((client->namespace = strdup(BGPVIEW_IO_KAFKA_NAMESPACE_DEFAULT)) ==
      NULL) {
    fprintf(stderr, "Failed to duplicate namespace string\n");
    goto err;
  }
  if ((client->brokers = strdup(BGPVIEW_IO_KAFKA_BROKER_URI_DEFAULT)) == NULL) {
    fprintf(stderr, "Failed to duplicate kafka server uri string\n");
    goto err;
  }

  if (opts != NULL && (len = strlen(opts)) > 0) {
    /* parse the option string ready for getopt */
    local_args = strdup(opts);
    parse_cmd(local_args, &process_argc, process_argv, MAXOPTS, "kafka");
    /* now parse the arguments using getopt */
    if (parse_args(client, process_argc, process_argv) != 0) {
      goto err;
    }
  }

  if (client->mode == BGPVIEW_IO_KAFKA_MODE_AUTO_CONSUMER) {
    if (client->identity == NULL) {
      client->mode = BGPVIEW_IO_KAFKA_MODE_GLOBAL_CONSUMER;
    } else {
      client->mode = BGPVIEW_IO_KAFKA_MODE_DIRECT_CONSUMER;
    }
  }

  /* check that mandatory opts have been set */
  if (client->identity != NULL) {
    assert(strlen(client->identity) < IDENTITY_MAX_LEN);
    if (client->mode == BGPVIEW_IO_KAFKA_MODE_GLOBAL_CONSUMER) {
      fprintf(stderr,
              "WARN: Identity string is not used for the global consumer\n");
    }
  } else if (client->mode != BGPVIEW_IO_KAFKA_MODE_GLOBAL_CONSUMER) {
    fprintf(stderr,
            "ERROR: Identity must be set for producer and direct consumer\n");
    usage();
    goto err;
  }

  if (client->mode == BGPVIEW_IO_KAFKA_MODE_GLOBAL_CONSUMER) {
    if ((client->gc_state.topics = kh_init(str_topic)) == NULL) {
      goto err;
    }
#ifdef WITH_THREADS
    pthread_mutex_init(&client->gc_state.mutex, NULL);
#endif
  }

  free(local_args);
  return client;

err:
  free(local_args);
  bgpview_io_kafka_destroy(client);
  return NULL;
}

void bgpview_io_kafka_destroy(bgpview_io_kafka_t *client)
{
  if (client == NULL) {
    return;
  }

  if (client->rdk_conn != NULL) {
    int drain_wait_cnt = 12;
    while (rd_kafka_outq_len(client->rdk_conn) > 0 && drain_wait_cnt > 0) {
      fprintf(
        stderr,
        "INFO: Waiting for Kafka queue to drain (currently %d messages)\n",
        rd_kafka_outq_len(client->rdk_conn));
      rd_kafka_poll(client->rdk_conn, 5000);
      drain_wait_cnt--;
    }

    // if this is a producer, tell the members topic we're going away
    if (client->mode == BGPVIEW_IO_KAFKA_MODE_PRODUCER) {
      bgpview_io_kafka_producer_send_members_update(client, 0);
    }
  }

  free(client->brokers);
  client->brokers = NULL;

  free(client->identity);
  client->identity = NULL;

  free(client->namespace);
  client->namespace = NULL;

  free(client->channel);
  client->channel = NULL;

  fprintf(stderr, "INFO: Shutting down topics\n");
  bgpview_io_kafka_topic_id_t id;
  for (id = 0; id < BGPVIEW_IO_KAFKA_TOPIC_ID_CNT; id++) {
    if (RKT(id) != NULL) {
      rd_kafka_topic_destroy(RKT(id));
      RKT(id) = NULL;
    }
  }

  if (client->mode == BGPVIEW_IO_KAFKA_MODE_GLOBAL_CONSUMER) {
    fprintf(stderr, "INFO: Destroying global consumer state\n");
    if (client->gc_state.topics != NULL) {
      kh_free_vals(str_topic, client->gc_state.topics, free_gc_topics);
      kh_free(str_topic, client->gc_state.topics, (void (*)(char *))free);
      kh_destroy(str_topic, client->gc_state.topics);
      client->gc_state.topics = NULL;
    }
#ifdef WITH_THREADS
    pthread_mutex_destroy(&client->gc_state.mutex);
#endif
  }

  free(client->dc_state.idmap.map);
  client->dc_state.idmap.map = NULL;
  client->dc_state.idmap.alloc_cnt = 0;

  fprintf(stderr, "INFO: Shutting down rdkafka\n");
  if (client->rdk_conn != NULL) {
    rd_kafka_destroy(client->rdk_conn);
    client->rdk_conn = NULL;
  }

  free(client);
  return;
}

typedef int(kafka_connect_func_t)(bgpview_io_kafka_t *client);

static kafka_connect_func_t *kafka_connect_funcs[] = {
  // BGPVIEW_IO_KAFKA_MODE_DIRECT_CONSUMER
  bgpview_io_kafka_consumer_connect,

  // BGPVIEW_IO_KAFKA_MODE_GLOBAL_CONSUMER
  bgpview_io_kafka_consumer_connect,

  // AUTO CONSUMER
  NULL,

  // BGPVIEW_IO_KAFKA_MODE_PRODUCER
  bgpview_io_kafka_producer_connect,
};

int bgpview_io_kafka_start(bgpview_io_kafka_t *client)
{
  int wait = 10;
  int connect_retries = BGPVIEW_IO_KAFKA_CONNECT_MAX_RETRIES;

  while (client->connected == 0 && connect_retries > 0) {
    if (kafka_connect_funcs[client->mode](client) != 0) {
      goto err;
    }

    connect_retries--;
    if (client->connected == 0 && connect_retries > 0) {
      fprintf(stderr,
              "WARN: Failed to connect to Kafka. Retrying in %d seconds\n",
              wait);
      sleep(wait);
      wait *= 2;
      if (wait > 180) {
        wait = 180;
      }
    }
  }

  if (client->connected == 0) {
    fprintf(stderr,
            "ERROR: Failed to connect to Kafka after %d retries. Giving up\n",
            BGPVIEW_IO_KAFKA_CONNECT_MAX_RETRIES);
    goto err;
  }

  // connect to topics (esp for members topic)
  if (kafka_topic_connect(client) != 0) {
    return -1;
  }

  if (client->mode == BGPVIEW_IO_KAFKA_MODE_PRODUCER) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    bgpview_io_kafka_producer_send_members_update(client, tv.tv_sec);
  }

  return 0;

err:
  return -1;
}

int bgpview_io_kafka_set_broker_addresses(bgpview_io_kafka_t *client,
                                          const char *addresses)
{
  if (client->brokers != NULL) {
    free(client->brokers);
  }

  if ((client->brokers = strdup(addresses)) == NULL) {
    fprintf(stderr, "Could not set broker addresses\n");
    return -1;
  }

  return 0;
}

int bgpview_io_kafka_set_namespace(bgpview_io_kafka_t *client,
                                   const char *namespace)
{
  if (client->namespace != NULL) {
    free(client->namespace);
  }

  if ((client->namespace = strdup(namespace)) == NULL) {
    fprintf(stderr, "Could not set namespace\n");
    return -1;
  }

  return 0;
}

int bgpview_io_kafka_send_view(bgpview_io_kafka_t *client, bgpview_t *view,
                               bgpview_t *parent_view,
                               bgpview_io_filter_cb_t *cb, void *cb_user)
{
  // first, ensure all topics are connected
  if (kafka_topic_connect(client) != 0) {
    return -1;
  }
  return bgpview_io_kafka_producer_send(client, view, parent_view, cb, cb_user);
}

int bgpview_io_kafka_recv_view(bgpview_io_kafka_t *client, bgpview_t *view,
                               bgpview_io_filter_peer_cb_t *peer_cb,
                               bgpview_io_filter_pfx_cb_t *pfx_cb,
                               bgpview_io_filter_pfx_peer_cb_t *pfx_peer_cb)

{
  // first, ensure all topics are connected
  if (kafka_topic_connect(client) != 0) {
    return -1;
  }
  return bgpview_io_kafka_consumer_recv(client, view, peer_cb, pfx_cb,
                                        pfx_peer_cb);
}

bgpview_io_kafka_stats_t *bgpview_io_kafka_get_stats(bgpview_io_kafka_t *client)
{
  return &client->prod_state.stats;
}
