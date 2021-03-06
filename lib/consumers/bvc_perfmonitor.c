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

#include "bvc_perfmonitor.h"
#include "bgpview_consumer_interface.h"
#include "bgpstream_utils.h"
#include "utils.h"
#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#define NAME "perfmonitor"

#define BUFFER_LEN 1024
#define META_METRIC_PREFIX_FORMAT "%s.meta.bgpview.consumer." NAME

#define DUMP_METRIC(value, time, fmt, ...)                                     \
  do {                                                                         \
    char buf[1024];                                                            \
    snprintf(buf, 1024, META_METRIC_PREFIX_FORMAT "." fmt, __VA_ARGS__);       \
    timeseries_set_single(BVC_GET_TIMESERIES(consumer), buf, value, time);     \
  } while (0)

#define STATE (BVC_GET_STATE(consumer, perfmonitor))

#define CHAIN_STATE (BVC_GET_CHAIN_STATE(consumer))

static bvc_t bvc_perfmonitor = {BVC_ID_PERFMONITOR, NAME,
                                BVC_GENERATE_PTRS(perfmonitor)};

typedef struct bvc_perfmonitor_state {

  /** The number of views we have processed */
  int view_cnt;

  /** Timeseries Key Package (general) */
  timeseries_kp_t *kp_gen;

} bvc_perfmonitor_state_t;

#if 0
/** Print usage information to stderr */
static void usage(bvc_t *consumer)
{
  fprintf(stderr,
	  "consumer usage: %s\n",
	  /*	  "       -c <level>    output compression level to use (default: %d)\n" */
	  consumer->name);
}
#endif

static char *graphite_safe(char *p)
{
  if (p == NULL) {
    return p;
  }

  char *r = p;
  while (*p != '\0') {
    if (*p == '.') {
      *p = '_';
    }
    if (*p == '*') {
      *p = '-';
    }
    p++;
  }
  return r;
}

/** Parse the arguments given to the consumer */
static int parse_args(bvc_t *consumer, int argc, char **argv)
{
  /*int opt;*/

  assert(argc > 0 && argv != NULL);

  /* NB: remember to reset optind to 1 before using getopt! */
  optind = 1;

  return 0;
}

bvc_t *bvc_perfmonitor_alloc()
{
  return &bvc_perfmonitor;
}

int bvc_perfmonitor_init(bvc_t *consumer, int argc, char **argv)
{
  bvc_perfmonitor_state_t *state = NULL;

  if ((state = malloc_zero(sizeof(bvc_perfmonitor_state_t))) == NULL) {
    return -1;
  }
  BVC_SET_STATE(consumer, state);

  /* set defaults here */

  state->view_cnt = 0;

  /* parse the command line args */
  if (parse_args(consumer, argc, argv) != 0) {
    return -1;
  }

  /* react to args here */

  return 0;
}

void bvc_perfmonitor_destroy(bvc_t *consumer)
{
  bvc_perfmonitor_state_t *state = STATE;

  fprintf(stderr, "BWC-TEST: %d views processed\n", STATE->view_cnt);

  if (state == NULL) {
    return;
  }

  /* destroy things here */

  free(state);

  BVC_SET_STATE(consumer, NULL);
}

int bvc_perfmonitor_process_view(bvc_t *consumer, bgpview_t *view)
{
  // view arrival delay, i.e. now - table ts
  uint32_t time_begin = epoch_sec();
  DUMP_METRIC(time_begin - bgpview_get_time(view), bgpview_get_time(view), "%s",
              CHAIN_STATE->metric_prefix, "view_arrival_delay");

  // get the number of peers and their table size

  bgpview_iter_t *it;

  /* create a new iterator */
  if ((it = bgpview_iter_create(view)) == NULL) {
    return -1;
  }

  bgpstream_peer_sig_t *sig;
  unsigned long long pfx4_cnt;
  unsigned long long pfx6_cnt;
  unsigned long long peer_on = 1;

  char addr[INET6_ADDRSTRLEN] = "";

  for (bgpview_iter_first_peer(it, BGPVIEW_FIELD_ACTIVE);
       bgpview_iter_has_more_peer(it); bgpview_iter_next_peer(it)) {
    /* grab the peer id */
    sig = bgpview_iter_peer_get_sig(it);
    assert(sig != NULL);
    pfx4_cnt = bgpview_iter_peer_get_pfx_cnt(it, BGPSTREAM_ADDR_VERSION_IPV4,
                                             BGPVIEW_FIELD_ACTIVE);
    pfx6_cnt = bgpview_iter_peer_get_pfx_cnt(it, BGPSTREAM_ADDR_VERSION_IPV6,
                                             BGPVIEW_FIELD_ACTIVE);

    bgpstream_addr_ntop(addr, INET6_ADDRSTRLEN, &(sig->peer_ip_addr));
    graphite_safe(addr);
    DUMP_METRIC(peer_on, bgpview_get_time(view), "peers.%s.%s.peer_on",
                CHAIN_STATE->metric_prefix, sig->collector_str, addr);

    DUMP_METRIC(pfx4_cnt, bgpview_get_time(view), "peers.%s.%s.ipv4_cnt",
                CHAIN_STATE->metric_prefix, sig->collector_str, addr);

    DUMP_METRIC(pfx6_cnt, bgpview_get_time(view), "peers.%s.%s.ipv6_cnt",
                CHAIN_STATE->metric_prefix, sig->collector_str, addr);
  }

  /* destroy the view iterator */
  bgpview_iter_destroy(it);

  STATE->view_cnt++;

  uint32_t time_end = epoch_sec();

  DUMP_METRIC(time_end - time_begin, bgpview_get_time(view), "%s",
              CHAIN_STATE->metric_prefix, "processing_time");

  return 0;
}
