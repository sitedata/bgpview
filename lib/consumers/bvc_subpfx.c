/*
 * This file is part of bgpstream
 *
 * CAIDA, UC San Diego
 * bgpstream-info@caida.org
 *
 * Copyright (C) 2012 The Regents of the University of California.
 * Authors: Alistair King, Chiara Orsini
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation; either version 2 of the License, or (at your option) any later
 * version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "bgpview_consumer_interface.h"
#include "bgpstream_utils_patricia.h"
#include "khash.h"
#include "utils.h"
#include "wandio.h"
#include "wandio_utils.h"
#include "bvc_subpfx.h"

#define NAME "subpfx"

/* macro to access the current consumer state */
#define STATE (BVC_GET_STATE(consumer, subpfx))

/* macro to access the current chain state, i.e.
 * the state variables shared by other consumers */
#define CHAIN_STATE (BVC_GET_CHAIN_STATE(consumer))

#define CUR_SUBPFXS (STATE->subpfxs[STATE->current_subpfxs_idx])
#define PREV_SUBPFXS (STATE->subpfxs[(STATE->current_subpfxs_idx + 1) % 2])

#define DEFAULT_OUTPUT_DIR "./"
#define DEFAULT_COMPRESS_LEVEL 6
#define OUTPUT_FILE_FORMAT "%s/" NAME ".%" PRIu32 ".events.gz"
#define BUFFER_LEN 4096

/* Maps sub-prefixes to super prefixes */
KHASH_INIT(pfx2pfx, bgpstream_pfx_storage_t, bgpstream_pfx_storage_t, 1,
           bgpstream_pfx_storage_hash_val, bgpstream_pfx_storage_equal_val);

enum {
  NEW = 0,
  FINISHED = 1,
};

static char *diff_type_strs[] = {
  "NEW",
  "FINISHED",
};

/* our 'class' */
static bvc_t bvc_subpfx = { //
  BVC_ID_SUBPFX,
  NAME, //
  BVC_GENERATE_PTRS(subpfx) //
};

/* our 'instance' */
typedef struct bvc_subpfx_state {

  // options:
  char *outdir;

  // Patricia tree used to find sub-prefixes in the current view
  bgpstream_patricia_tree_t *pt;

  // Re-usable result set used when finding parent prefix
  bgpstream_patricia_tree_result_set_t *pt_res;

  // Flip-flop buffer for current and previous sub-prefix to super-prefix maps
  khash_t(pfx2pfx) *subpfxs[2];

  // which subpfxs map should be filled for this view
  // ((current_subpfxs_idx+1)%2) is the map for the previous view
  int current_subpfxs_idx;

  // current output file name
  char outfile_name[BUFFER_LEN];

  // current output file handle
  iow_t *outfile;

} bvc_subpfx_state_t;

/** Print usage information to stderr */
static void usage(bvc_t *consumer)
{
  fprintf(stderr, "consumer usage: %s\n"
          "       -o <output-dir>      output directory (default: %s)\n",
          consumer->name,
          DEFAULT_OUTPUT_DIR);
}

/** Parse the arguments given to the consumer */
static int parse_args(bvc_t *consumer, int argc, char **argv)
{
  int opt;

  assert(argc > 0 && argv != NULL);

  /* NB: remember to reset optind to 1 before using getopt! */
  optind = 1;

  /* remember the argv strings DO NOT belong to us */
  while ((opt = getopt(argc, argv, ":o:?")) >= 0) {
    switch (opt) {
    case 'o':
      STATE->outdir = strdup(optarg);
      assert(STATE->outdir != NULL);
      break;

    case '?':
    case ':':
    default:
      usage(consumer);
      return -1;
    }
  }

  return 0;
}

static void find_subpfxs(bgpstream_patricia_tree_t *pt,
                         bgpstream_patricia_node_t *node, void *data)
{
  bvc_t *consumer = (bvc_t*)data;
  bgpstream_pfx_t *pfx = bgpstream_patricia_tree_get_pfx(node);

  // does this prefix have a super-prefix?
  if (bgpstream_patricia_tree_get_mincovering_prefix(pt, node,
                                                     STATE->pt_res) != 0) {
    // TODO: change the patricia tree walk func to allow me to error out!
    assert(0);
  }
  bgpstream_patricia_node_t *super_node =
    bgpstream_patricia_tree_result_set_next(STATE->pt_res);

  if (super_node == NULL) {
    // this is not a sub-prefix
    return;
  }
  bgpstream_pfx_t *super_pfx = bgpstream_patricia_tree_get_pfx(super_node);

  bgpstream_pfx_storage_t tmp_pfx;
  bgpstream_pfx_copy((bgpstream_pfx_t*)&tmp_pfx, pfx);
  bgpstream_pfx_storage_t tmp_super_pfx;
  bgpstream_pfx_copy((bgpstream_pfx_t*)&tmp_super_pfx, super_pfx);

  // this is a sub-prefix, add it to our table
  int ret;
  int k = kh_put(pfx2pfx, CUR_SUBPFXS, tmp_pfx, &ret);
  assert(ret > 0); // this prefix must not already be present in the map
  kh_val(CUR_SUBPFXS, k) = tmp_super_pfx;
}

static int dump_as_paths(bvc_t *consumer, bgpview_t *view, bgpview_iter_t *it,
                         bgpstream_pfx_t *pfx)
{
  bgpstream_as_path_t *path;
  char path_str[BUFFER_LEN];

  // seek the iterator to this prefix (it is guaranteed to be in the view)
  if (bgpview_iter_seek_pfx(it, pfx, BGPVIEW_FIELD_ACTIVE) == 0) {
    fprintf(stderr, "ERROR: failed to find prefix in view\n");
    return -1;
  }

  int first_path = 1;
  // spin through the peers for this prefix and dump out their AS paths
  for (bgpview_iter_pfx_first_peer(it, BGPVIEW_FIELD_ACTIVE); //
       bgpview_iter_pfx_has_more_peer(it); //
       bgpview_iter_pfx_next_peer(it)) {
    if (first_path == 0) {
      wandio_printf(STATE->outfile, ":");
    }

    // write this AS path out
    // TODO: optimize this to not construct an AS path each time
    path = bgpview_iter_pfx_peer_get_as_path(it);
    assert(path != NULL);
    bgpstream_as_path_snprintf(path_str, BUFFER_LEN, path);
    bgpstream_as_path_destroy(path);

    wandio_printf(STATE->outfile, "%s", path_str);

    first_path = 0;
  }

  return 0;
}

static int dump_subpfx(bvc_t *consumer, bgpview_t *view, bgpview_iter_t *it,
                       bgpstream_pfx_storage_t *pfx,
                       bgpstream_pfx_storage_t *super_pfx,
                       int diff_type)
{
  /* output file format: */
  /* TIME|SUPER_PFX|SUB_PFX|NEW/FINISHED|SUPER_PFX_PATHS|SUB_PFX_PATHS */
  /* NB: in FINISHED events, the PATHS fields will be empty */
  /* since AS path strings can contain commas, the AS paths with be
     colon-separated, e.g.:
     AS1 AS2 {AS3,AS4}:AS1 AS2 AS5
  */

  char pfx_str[INET6_ADDRSTRLEN+3];
  bgpstream_pfx_snprintf(pfx_str, INET6_ADDRSTRLEN + 3, (bgpstream_pfx_t*)pfx);
  char super_pfx_str[INET6_ADDRSTRLEN+3];
  bgpstream_pfx_snprintf(super_pfx_str, INET6_ADDRSTRLEN + 3,
                         (bgpstream_pfx_t*)super_pfx);

  wandio_printf(STATE->outfile,
                "%"PRIu32"|%s|%s|%s|",
                bgpview_get_time(view),
                super_pfx_str,
                pfx_str,
                diff_type_strs[diff_type]);

  if (diff_type == NEW) {
    // dump the AS paths
    dump_as_paths(consumer, view, it, (bgpstream_pfx_t*)super_pfx);
    wandio_printf(STATE->outfile, "|");
    dump_as_paths(consumer, view, it, (bgpstream_pfx_t*)pfx);
    wandio_printf(STATE->outfile, "\n");
  } else {
    // just finish the record with nulls
    wandio_printf(STATE->outfile, "|\n");
  }

  return 0;
}

static int subpfxs_diff(bvc_t *consumer, bgpview_t *view, bgpview_iter_t *it,
                        khash_t(pfx2pfx) *a, khash_t(pfx2pfx) *b, int diff_type)
{
  khiter_t k;
  for (k = kh_begin(a); k < kh_end(a); k++) {
    if (kh_exist(a, k) == 0) {
      continue;
    }
    bgpstream_pfx_storage_t *pfx = &kh_key(a, k);
    if (kh_get(pfx2pfx, b, *pfx) != kh_end(b)) {
      // this was in the previous view
      continue;
    }

    // this is a new/finished sub-pfx!
    if (dump_subpfx(consumer, view, it, pfx, &kh_val(a, k), diff_type) != 0) {
      return -1;
    }
  }
  return 0;
}

/* ==================== CONSUMER INTERFACE FUNCTIONS ==================== */

bvc_t *bvc_subpfx_alloc()
{
  return &bvc_subpfx;
}

int bvc_subpfx_init(bvc_t *consumer, int argc, char **argv)
{
  bvc_subpfx_state_t *state = NULL;

  if ((state = malloc_zero(sizeof(bvc_subpfx_state_t))) == NULL) {
    return -1;
  }
  BVC_SET_STATE(consumer, state);

  if ((state->pt = bgpstream_patricia_tree_create(NULL)) == NULL) {
    fprintf(stderr, "ERROR: Could not create patricia tree\n");
    goto err;
  }

  if ((STATE->pt_res = bgpstream_patricia_tree_result_set_create()) == NULL) {
    fprintf(stderr, "ERROR: Could not create patricia tree result set\n");
    goto err;
  }

  int i;
  for (i=0; i<2; i++) {
    if ((STATE->subpfxs[i] = kh_init(pfx2pfx)) == NULL) {
      fprintf(stderr, "ERROR: Could not create subpfx map\n");
      goto err;
    }
  }
  STATE->current_subpfxs_idx = 0;

  /* parse the command line args */
  if (parse_args(consumer, argc, argv) != 0) {
    goto err;
  }

  return 0;

err:
  return -1;
}

void bvc_subpfx_destroy(bvc_t *consumer)
{
  bvc_subpfx_state_t *state = STATE;

  if (state == NULL) {
    return;
  }

  free(STATE->outdir);
  STATE->outdir = NULL;

  bgpstream_patricia_tree_destroy(state->pt);
  state->pt = NULL;
  bgpstream_patricia_tree_result_set_destroy(&state->pt_res);

  int i;
  for (i=0; i<2; i++) {
    kh_destroy(pfx2pfx, state->subpfxs[i]);
    state->subpfxs[i] = NULL;
  }

  free(state);

  BVC_SET_STATE(consumer, NULL);
}

int bvc_subpfx_process_view(bvc_t *consumer, bgpview_t *view)
{
  bgpview_iter_t *it = NULL;

  // open the output file
  snprintf(STATE->outfile_name, BUFFER_LEN, OUTPUT_FILE_FORMAT,
           STATE->outdir, bgpview_get_time(view));
  if ((STATE->outfile = wandio_wcreate(STATE->outfile_name,
                                       wandio_detect_compression_type(STATE->outfile_name),
                                       DEFAULT_COMPRESS_LEVEL, O_CREAT)) == NULL) {
    fprintf(stderr, "ERROR: Could not open %s for writing\n",
            STATE->outfile_name);
    goto err;
  }

  /* create a new iterator */
  if ((it = bgpview_iter_create(view)) == NULL) {
    return -1;
  }

  /* build the patricia tree of prefixes in the current view */
  /* TODO: add a utility function to the view which constructs this so it can be
     reused by others */
  for (bgpview_iter_first_pfx(it, 0 /* all ip versions*/, BGPVIEW_FIELD_ACTIVE);
       bgpview_iter_has_more_pfx(it); //
       bgpview_iter_next_pfx(it)) {
    // we have to walk through the peers to see if this prefix is announced by
    // at least one FF peer
    bgpstream_pfx_t *pfx = bgpview_iter_pfx_get_pfx(it);
    int ipv_idx = bgpstream_ipv2idx(pfx->address.version);
    int is_ff = 0;
    for (bgpview_iter_pfx_first_peer(it, BGPVIEW_FIELD_ACTIVE); //
         bgpview_iter_pfx_has_more_peer(it); //
         bgpview_iter_pfx_next_peer(it)) {
      if (bgpstream_id_set_exists(
            BVC_GET_CHAIN_STATE(consumer)->full_feed_peer_ids[ipv_idx],
            bgpview_iter_peer_get_peer_id(it))) {
        is_ff = 1;
        break;
      }
    }
    if (is_ff != 0 && bgpstream_patricia_tree_insert(STATE->pt, pfx) == NULL) {
      fprintf(stderr, "ERROR: Could not insert prefix in patricia tree\n");
      goto err;
    }
  }

  /* iterate through the prefixes in the tree and find the sub-prefixes */
  bgpstream_patricia_tree_walk(STATE->pt, find_subpfxs, consumer);

  // now that we have a table of sub-prefixes, find out which are new
  // (i.e., which are in this view but not in the previous one)
  if (subpfxs_diff(consumer, view, it, CUR_SUBPFXS, PREV_SUBPFXS, NEW) != 0) {
    fprintf(stderr, "ERROR: Failed to find NEW sub prefixes\n");
    goto err;
  }
  // and then do the complement to find finished sub-pfxs
  if (subpfxs_diff(consumer, view, it,
                   PREV_SUBPFXS, CUR_SUBPFXS, FINISHED) != 0) {
    fprintf(stderr, "ERROR: Failed to find NEW sub prefixes\n");
    goto err;
  }

  // clear the previous map and then rotate
  kh_clear(pfx2pfx, PREV_SUBPFXS);
  STATE->current_subpfxs_idx = (STATE->current_subpfxs_idx + 1) % 2;

  /* destroy the view iterator */
  bgpview_iter_destroy(it);

  /* empty the patricia tree */
  bgpstream_patricia_tree_clear(STATE->pt);

  /* close the output file */
  wandio_wdestroy(STATE->outfile);
  STATE->outfile = NULL;

  /* generate the .done file */
  snprintf(STATE->outfile_name, BUFFER_LEN, OUTPUT_FILE_FORMAT ".done",
           STATE->outdir, bgpview_get_time(view));
  if ((STATE->outfile = wandio_wcreate(STATE->outfile_name,
                                       wandio_detect_compression_type(STATE->outfile_name),
                                       DEFAULT_COMPRESS_LEVEL, O_CREAT)) == NULL) {
    fprintf(stderr, "ERROR: Could not open %s for writing\n", STATE->outfile_name);
    return -1;
  }
  wandio_wdestroy(STATE->outfile);
  STATE->outfile = NULL;

  return 0;

 err:
  bgpview_iter_destroy(it);
  wandio_wdestroy(STATE->outfile);
  return -1;
}