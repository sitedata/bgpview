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
#include "bgpview_io.h"
#include "bgpview_io_file.h"
#include "config.h"
#include "utils.h"
#include <arpa/inet.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <wandio.h>

#define VIEW_MAGIC 0x42475056 /* BGPV */

#define VIEW_START_MAGIC 0x53545254    /* STRT */
#define VIEW_END_MAGIC 0x56454E44      /* VEND */
#define VIEW_PEER_END_MAGIC 0x50454E44 /* PEND */
#define VIEW_PATH_END_MAGIC 0x50415448 /* PATH */
#define VIEW_PFX_END_MAGIC 0x58454E44  /* XEND */

#define BUFFER_LEN 1024

/* ========== UTILITIES ========== */

#define WRITE_VAL(from)                                                        \
  do {                                                                         \
    if (wandio_wwrite(outfile, &from, sizeof(from)) != sizeof(from)) {         \
      fprintf(stderr, "%s: Could not write %s to file\n", __func__,            \
              STR(from));                                                      \
    }                                                                          \
  } while (0)

#define WRITE_MAGIC(magic)                                                     \
  do {                                                                         \
    uint32_t mgc = htonl(VIEW_MAGIC);                                          \
    WRITE_VAL(mgc);                                                            \
    mgc = htonl(magic);                                                        \
    WRITE_VAL(mgc);                                                            \
  } while (0)

#define READ_VAL(to)                                                           \
  do {                                                                         \
    if (wandio_read(infile, &to, sizeof(to)) != sizeof(to)) {                  \
      fprintf(stderr, "%s: Could not read %s from file\n", __func__, STR(to)); \
    }                                                                          \
  } while (0)

/** Checks if the given magic number is present in the file. If it is, the magic
    is consumed, otherwise the stream is left untouched */
static int check_magic(io_t *infile, uint32_t magic)
{
  uint64_t buf;
  uint32_t mgc;
  off_t read;
  if (wandio_peek(infile, &buf, sizeof(uint64_t)) != sizeof(uint64_t)) {
    fprintf(stderr, "Could not peek at bytes\n");
    return 0;
  }

  buf = ntohll(buf);

  /* check the generic magic */
  mgc = (uint32_t)(buf >> 32);
  if (mgc != VIEW_MAGIC) {
    return 0;
  }

  /* now, check the specific magic */
  mgc = (buf & 0xffffffff);
  if (mgc != magic) {
    return 0;
  }

  /* now consume the magic! */
  read = wandio_read(infile, &buf, sizeof(uint64_t));
  assert(read == sizeof(uint64_t));

  return 1;
}

static int write_ip(iow_t *outfile, bgpstream_ip_addr_t *ip)
{
  uint8_t len;
  switch (ip->version) {
  case BGPSTREAM_ADDR_VERSION_IPV4:
    len = sizeof(uint32_t);
    WRITE_VAL(len);
    if (wandio_wwrite(outfile, &ip->bs_ipv4.addr.s_addr, len) == len) {
      return 0;
    }
    break;

  case BGPSTREAM_ADDR_VERSION_IPV6:
    len = sizeof(uint8_t) * 16;
    WRITE_VAL(len);
    if (wandio_wwrite(outfile, &ip->bs_ipv6.addr.s6_addr, len) == len) {
      return 0;
    }
    break;

  case BGPSTREAM_ADDR_VERSION_UNKNOWN:
    return -1;
  }

  return -1;
}

static int read_ip(io_t *infile, bgpstream_ip_addr_t *ip)
{
  assert(ip != NULL);

  uint8_t len;
  READ_VAL(len);

  /* 4 bytes means ipv4, 16 means ipv6 */
  if (len == sizeof(uint32_t)) {
    /* v4 */
    ip->version = BGPSTREAM_ADDR_VERSION_IPV4;
    if (wandio_read(infile, &ip->bs_ipv4.addr.s_addr, len) != len) {
      goto err;
    }
  } else if (len == sizeof(uint8_t) * 16) {
    /* v6 */
    ip->version = BGPSTREAM_ADDR_VERSION_IPV6;
    if (wandio_read(infile, &ip->bs_ipv6.addr.s6_addr, len) != len) {
      goto err;
    }
  } else {
    /* invalid ip address */
    fprintf(stderr, "Invalid IP address (len: %d)\n", len);
    goto err;
  }

  return 0;

err:
  return -1;
}

static int write_peers(iow_t *outfile, bgpview_iter_t *it,
                       bgpview_io_filter_cb_t *cb, void *cb_user)
{
  uint8_t u8;
  uint16_t u16;
  uint32_t u32;
  int siglen;

  bgpstream_peer_sig_t *ps;

  int peers_tx = 0;

  int filter = 0;

  bgpview_t *view = bgpview_iter_get_view(it);
  assert(view != NULL);

  /* foreach peer, send peerid, collector string, peer ip (version, address),
     peer asn */
  for (bgpview_iter_first_peer(it, BGPVIEW_FIELD_ACTIVE);
       bgpview_iter_has_more_peer(it); bgpview_iter_next_peer(it)) {
    if (cb != NULL) {
      /* ask the caller if they want this peer */
      if ((filter = cb(it, BGPVIEW_IO_FILTER_PEER, cb_user)) < 0) {
        goto err;
      }
      if (filter == 0) {
        continue;
      }
    }

    /* past here means this peer is being sent */
    peers_tx++;

    /* peer id */
    u16 = bgpview_iter_peer_get_peer_id(it);
    u16 = htons(u16);
    WRITE_VAL(u16);

    ps = bgpview_iter_peer_get_sig(it);
    assert(ps);
    siglen = strlen(ps->collector_str);
    assert(siglen <= UINT8_MAX);
    u8 = siglen;
    WRITE_VAL(u8);
    if (wandio_wwrite(outfile, &ps->collector_str, u8) != u8) {
      goto err;
    }

    /* peer IP address */
    if (write_ip(outfile, &ps->peer_ip_addr) != 0) {
      goto err;
    }

    /* peer AS number */
    u32 = ps->peer_asnumber;
    u32 = htonl(u32);
    WRITE_VAL(u32);
  }

  /* write end-of-peers magic number */
  WRITE_MAGIC(VIEW_PEER_END_MAGIC);

  /* now send the number of peers for cross validation */
  assert(peers_tx <= UINT16_MAX);
  u16 = htons(peers_tx);
  WRITE_VAL(u16);

  return 0;

err:
  return -1;
}

static int write_paths(iow_t *outfile, bgpview_iter_t *it)
{
  bgpview_t *view = bgpview_iter_get_view(it);
  assert(view != NULL);
  bgpstream_as_path_store_t *ps = bgpview_get_as_path_store(view);
  assert(ps != NULL);

  bgpstream_as_path_store_path_t *spath;
  bgpstream_as_path_t *path;
  uint32_t idx;

  uint8_t *path_data;
  uint8_t is_core;
  uint16_t path_len;

  unsigned paths_tx = 0;
  uint32_t u32;

  /* foreach path, send pathid and path */
  for (bgpstream_as_path_store_iter_first_path(ps);
       bgpstream_as_path_store_iter_has_more_path(ps);
       bgpstream_as_path_store_iter_next_path(ps)) {
    paths_tx++;
    spath = bgpstream_as_path_store_iter_get_path(ps);
    assert(spath != NULL);

    idx = bgpstream_as_path_store_path_get_idx(spath);

    is_core = bgpstream_as_path_store_path_is_core(spath);

    path = bgpstream_as_path_store_path_get_int_path(spath);
    assert(path != NULL);
    path_len = bgpstream_as_path_get_data(path, &path_data);

    /* add the path index */
    WRITE_VAL(idx);

    /* is this a core path? */
    WRITE_VAL(is_core);

    /* add the path len */
    WRITE_VAL(path_len);

    /** @todo make platform independent (paths are in host byte order) */
    if (wandio_wwrite(outfile, path_data, path_len) != path_len) {
      goto err;
    }
  }

  /* write end-of-paths magic number */
  WRITE_MAGIC(VIEW_PATH_END_MAGIC);

  /* now send the number of paths for cross validation */
  assert(paths_tx <= UINT32_MAX);
  u32 = htonl(paths_tx);
  WRITE_VAL(u32);

  return 0;

err:
  return -1;
}

static int write_pfx_peers(iow_t *outfile, bgpview_iter_t *it, int *peers_cnt,
                           bgpview_io_filter_cb_t *cb, void *cb_user)
{
  uint16_t peerid;
  bgpstream_as_path_store_path_t *spath;
  uint32_t idx;

  int filter;

  assert(peers_cnt != NULL);
  *peers_cnt = 0;

  for (bgpview_iter_pfx_first_peer(it, BGPVIEW_FIELD_ACTIVE);
       bgpview_iter_pfx_has_more_peer(it); bgpview_iter_pfx_next_peer(it)) {
    if (cb != NULL) {
      /* ask the caller if they want this peer */
      if ((filter = cb(it, BGPVIEW_IO_FILTER_PFX_PEER, cb_user)) < 0) {
        return -1;
      }
      if (filter == 0) {
        continue;
      }
    }

    peerid = bgpview_iter_peer_get_peer_id(it);

    /* peer id */
    assert(peerid > 0);
    peerid = htons(peerid);
    WRITE_VAL(peerid);

    /* AS Path Index */
    spath = bgpview_iter_pfx_peer_get_as_path_store_path(it);
    idx = bgpstream_as_path_store_path_get_idx(spath);
    WRITE_VAL(idx);

    (*peers_cnt)++;
  }

  return 0;
}

static int write_pfxs(iow_t *outfile, bgpview_iter_t *it,
                      bgpview_io_filter_cb_t *cb, void *cb_user)
{
  int filter;

  uint16_t u16;
  uint32_t u32;

  /* the number of pfxs we actually sent */
  int pfx_cnt = 0;

  bgpstream_pfx_t *pfx;
  int peers_cnt = 0;

  for (bgpview_iter_first_pfx(it, 0, /* all pfx versions */
                              BGPVIEW_FIELD_ACTIVE);
       bgpview_iter_has_more_pfx(it); bgpview_iter_next_pfx(it)) {
    if (cb != NULL) {
      /* ask the caller if they want this peer */
      if ((filter = cb(it, BGPVIEW_IO_FILTER_PFX, cb_user)) < 0) {
        return -1;
      }
      if (filter == 0) {
        continue;
      }
    }

    pfx = bgpview_iter_pfx_get_pfx(it);
    assert(pfx != NULL);

    /* pfx address */
    if (write_ip(outfile, &pfx->address) != 0) {
      goto err;
    }

    /* pfx len */
    WRITE_VAL(pfx->mask_len);

    /* send the peers */
    peers_cnt = 0;
    if (write_pfx_peers(outfile, it, &peers_cnt, cb, cb_user) != 0) {
      goto err;
    }

    /* for a pfx to be sent it must have active peers */
    if (peers_cnt == 0) {
      continue;
    }

    /* write end-of-peers magic */
    WRITE_MAGIC(VIEW_PEER_END_MAGIC);

    /* peer cnt for cross validation */
    assert(peers_cnt > 0);
    u16 = htons(peers_cnt);
    WRITE_VAL(u16);

    pfx_cnt++;
  }

  /* write end-of-pfxs magic */
  WRITE_MAGIC(VIEW_PFX_END_MAGIC);

  /* send pfx cnt for cross-validation */
  u32 = htonl(pfx_cnt);
  WRITE_VAL(u32);

  return 0;

err:
  return -1;
}

static int read_peers(io_t *infile, bgpview_iter_t *iter,
                      bgpview_io_filter_peer_cb_t *peer_cb,
                      bgpstream_peer_id_t **peerid_mapping)
{
  uint16_t pc;
  int i, j;

  bgpstream_peer_id_t peerid_orig;
  bgpstream_peer_id_t peerid_new;

  bgpstream_peer_sig_t ps;
  uint8_t len;

  bgpstream_peer_id_t *idmap = NULL;
  int idmap_cnt = 0;

  int peers_rx = 0;

  int filter = 0;

  /* foreach peer, read peerid, collector string, peer ip (version, address),
     peer asn */
  for (i = 0; i < UINT16_MAX; i++) {
    /* peerid (or end-of-peers)*/
    if (check_magic(infile, VIEW_PEER_END_MAGIC) != 0) {
      /* end of peers */
      break;
    }

    READ_VAL(peerid_orig);
    peerid_orig = ntohs(peerid_orig);

    /* by here we have a valid peer to receive */
    peers_rx++;

    /* collector name */
    READ_VAL(len);
    if (wandio_read(infile, ps.collector_str, len) != len) {
      fprintf(stderr, "ERROR: Could not read collector name\n");
      goto err;
    }
    ps.collector_str[len] = '\0';

    /* peer ip */
    if (read_ip(infile, &ps.peer_ip_addr) != 0) {
      fprintf(stderr, "ERROR: Could not read peer ip\n");
      goto err;
    }

    /* peer asn */
    READ_VAL(ps.peer_asnumber);
    ps.peer_asnumber = ntohl(ps.peer_asnumber);

    if (iter == NULL) {
      continue;
    }
    /* all code below here has a valid view */

    if (peer_cb != NULL) {
      /* ask the caller if they want this peer */
      if ((filter = peer_cb(&ps)) < 0) {
        goto err;
      }
      if (filter == 0) {
        continue;
      }
    }

    /* ensure we have enough space in the id map */
    if ((peerid_orig + 1) > idmap_cnt) {
      if ((idmap = realloc(idmap, sizeof(bgpstream_peer_id_t) *
                                    (peerid_orig + 1))) == NULL) {
        goto err;
      }

      /* now set all ids to 0 (reserved) */
      for (j = idmap_cnt; j <= peerid_orig; j++) {
        idmap[j] = 0;
      }
      idmap_cnt = peerid_orig + 1;
    }

    /* now ask the view to add this peer */
    peerid_new = bgpview_iter_add_peer(iter, ps.collector_str,
                                       &ps.peer_ip_addr,
                                       ps.peer_asnumber);
    assert(peerid_new != 0);
    idmap[peerid_orig] = peerid_new;

    bgpview_iter_activate_peer(iter);
  }

  /* receive the number of peers */
  READ_VAL(pc);
  pc = ntohs(pc);
  assert(pc == peers_rx);

  *peerid_mapping = idmap;
  return idmap_cnt;

err:
  return -1;
}

static int read_paths(io_t *infile, bgpview_iter_t *iter,
                      bgpstream_as_path_store_path_id_t **pathid_mapping)
{
  uint32_t pc;

  uint32_t pathidx;
  uint16_t pathlen;
  uint8_t is_core;
  uint8_t pathdata[BUFFER_LEN];

  bgpstream_as_path_store_path_id_t *idmap = NULL;
  int idmap_cnt = 0;

  unsigned paths_rx = 0;

  bgpview_t *view = NULL;
  bgpstream_as_path_store_t *store = NULL;
  bgpstream_as_path_t *path = NULL;

  /* only if we have a valid iterator */
  if (iter != NULL) {
    view = bgpview_iter_get_view(iter);
    store = bgpview_get_as_path_store(view);

    /* create a path */
    if ((path = bgpstream_as_path_create()) == NULL) {
      return -1;
    }
  }

  /* loop until we find the path end magic number */
  while (paths_rx < UINT32_MAX) {
    /* pathid (or end-of-paths)*/
    if (check_magic(infile, VIEW_PATH_END_MAGIC) != 0) {
      /* end of peers */
      break;
    }

    /* by here we have a valid path to receive */
    paths_rx++;

    /* path idx */
    READ_VAL(pathidx);

    /* is core */
    READ_VAL(is_core);

    /* path len */
    READ_VAL(pathlen);

    /* path data */
    assert(pathlen <= BUFFER_LEN);
    if (wandio_read(infile, pathdata, pathlen) != pathlen) {
      fprintf(stderr, "ERROR: Could not read path data\n");
      goto err;
    }

    if (iter != NULL) {
      /* ensure we have enough space in the id map */
      if ((pathidx + 1) > idmap_cnt) {
        idmap_cnt = pathidx == 0 ? 1 : pathidx * 2;

        if ((idmap = realloc(idmap, sizeof(bgpstream_as_path_store_path_id_t) *
                                      idmap_cnt)) == NULL) {
          goto err;
        }

        /* WARN: ids are garbage */
      }

      /* now add this path to the store */
      if (bgpstream_as_path_store_insert_path(store, pathdata, pathlen, is_core,
                                              &idmap[pathidx]) != 0) {
        goto err;
      }
    }
  }

  /* receive the number of paths */
  READ_VAL(pc);
  pc = ntohl(pc);
  assert(pc == paths_rx);

  *pathid_mapping = idmap;
  return idmap_cnt;

err:
  return -1;
}

static int read_pfxs(io_t *infile, bgpview_iter_t *iter,
                     bgpview_io_filter_pfx_cb_t *pfx_cb,
                     bgpview_io_filter_pfx_peer_cb_t *pfx_peer_cb,
                     bgpstream_peer_id_t *peerid_map, int peerid_map_cnt,
                     bgpstream_as_path_store_path_id_t *pathid_map,
                     int pathid_map_cnt)
{
  uint32_t pfx_cnt;
  uint16_t peer_cnt;
  uint32_t i;
  uint16_t j;

  bgpstream_pfx_t pfx;
  bgpstream_peer_id_t peerid;

  uint32_t pathidx;

  int pfx_peers_added = 0;

  unsigned pfx_rx = 0;
  unsigned pfx_peer_rx = 0;

  int skip_pfx = 0;
  int filter;

  bgpview_t *view = NULL;
  bgpstream_as_path_store_t *store = NULL;
  bgpstream_as_path_store_path_t *store_path = NULL;
  if (iter != NULL) {
    view = bgpview_iter_get_view(iter);
    store = bgpview_get_as_path_store(view);
  }

  /* foreach pfx, read pfx.ip, pfx.len, [peers_cnt, peer_info] */
  for (i = 0; i < UINT32_MAX; i++) {
    if (check_magic(infile, VIEW_PFX_END_MAGIC) != 0) {
      /* end of pfxs */
      break;
    }
    pfx_rx++;
    skip_pfx = 0;

    /* pfx_ip */
    if (read_ip(infile, &pfx.address) != 0) {
      fprintf(stderr, "ERROR: Could not read pfx ip\n");
      goto err;
    }

    /* pfx len */
    READ_VAL(pfx.mask_len);

    if (pfx_cb != NULL) {
      /* ask the caller if they want this pfx */
      if ((filter = pfx_cb(&pfx)) < 0) {
        goto err;
      }
      if (filter == 0) {
        skip_pfx = 1;
      }
    }

    pfx_peers_added = 0;
    pfx_peer_rx = 0;

    for (j = 0; j < UINT16_MAX; j++) {
      if (check_magic(infile, VIEW_PEER_END_MAGIC) != 0) {
        /* end of peers */
        break;
      }

      /* peer id */
      READ_VAL(peerid);
      peerid = ntohs(peerid);

      pfx_peer_rx++;

      /* AS Path Index */
      READ_VAL(pathidx);

      if (iter == NULL || skip_pfx != 0) {
        continue;
      }
      /* all code below here has a valid iter */

      assert(peerid < peerid_map_cnt);

      if (pfx_peer_cb != NULL) {
        /* get the store path using the id */
        store_path =
          bgpstream_as_path_store_get_store_path(store, pathid_map[pathidx]);
        /* ask the caller if they want this pfx-peer */
        if ((filter = pfx_peer_cb(store_path)) < 0) {
          goto err;
        }
        if (filter == 0) {
          continue;
        }
      }

      if (pfx_peers_added == 0) {
        /* we have to use add_pfx_peer */
        if (bgpview_iter_add_pfx_peer_by_id(iter, &pfx,
                                            peerid_map[peerid],
                                            pathid_map[pathidx]) != 0) {
          fprintf(stderr, "Could not add prefix\n");
          goto err;
        }
      } else {
        /* we can use pfx_add_peer for efficiency */
        if (bgpview_iter_pfx_add_peer_by_id(iter, peerid_map[peerid],
                                            pathid_map[pathidx]) != 0) {
          fprintf(stderr, "Could not add prefix\n");
          goto err;
        }
      }

      pfx_peers_added++;

      /* now we have to activate it */
      if (bgpview_iter_pfx_activate_peer(iter) < 0) {
        fprintf(stderr, "Could not activate prefix\n");
        goto err;
      }
    }

    /* peer cnt */
    READ_VAL(peer_cnt);
    peer_cnt = ntohs(peer_cnt);
    assert(peer_cnt == pfx_peer_rx);
  }

  /* pfx cnt */
  READ_VAL(pfx_cnt);
  pfx_cnt = ntohl(pfx_cnt);
  assert(pfx_rx == pfx_cnt);

  return 0;

err:
  return -1;
}

/* ========== PUBLIC FUNCTIONS ========== */

int bgpview_io_file_write(iow_t *outfile, bgpview_t *view,
                          bgpview_io_filter_cb_t *cb, void *cb_user)
{
  uint32_t u32;
  bgpview_iter_t *it = NULL;

  if (view == NULL) {
    /* no-op */
    return 0;
  }

#ifdef DEBUG
  fprintf(stderr, "DEBUG: Writing view...\n");
#endif

  if ((it = bgpview_iter_create(view)) == NULL) {
    goto err;
  }

  /* start magic */
  WRITE_MAGIC(VIEW_START_MAGIC);

  /* time */
  u32 = htonl(bgpview_get_time(view));
  WRITE_VAL(u32);

  if (write_peers(outfile, it, cb, cb_user) != 0) {
    goto err;
  }

  if (write_paths(outfile, it) != 0) {
    goto err;
  }

  if (write_pfxs(outfile, it, cb, cb_user) != 0) {
    goto err;
  }

  /* write end-of-view magic number */
  WRITE_MAGIC(VIEW_END_MAGIC);

  bgpview_iter_destroy(it);

  return 0;

err:
  return -1;
}

int bgpview_io_file_read(io_t *infile, bgpview_t *view,
                         bgpview_io_filter_peer_cb_t *peer_cb,
                         bgpview_io_filter_pfx_cb_t *pfx_cb,
                         bgpview_io_filter_pfx_peer_cb_t *pfx_peer_cb)
{
  uint32_t u32;

  bgpstream_peer_id_t *peerid_map = NULL;
  int peerid_map_cnt = 0;

  bgpstream_as_path_store_path_id_t *pathid_map = NULL;
  int pathid_map_cnt;

  bgpview_iter_t *it = NULL;
  if (view != NULL && (it = bgpview_iter_create(view)) == NULL) {
    goto err;
  }

  /* check for eof */
  if (wandio_peek(infile, &u32, sizeof(u32)) == 0) {
    return 0;
  }

  if (check_magic(infile, VIEW_START_MAGIC) == 0) {
    fprintf(stderr, "ERROR: Missing view-start magic number\n");
    goto err;
  }

  /* time */
  READ_VAL(u32);
  if (view != NULL) {
    bgpview_set_time(view, ntohl(u32));
  }

  if ((peerid_map_cnt = read_peers(infile, it, peer_cb, &peerid_map)) < 0) {
    fprintf(stderr, "ERROR: Could not read peer table\n");
    goto err;
  }

  if ((pathid_map_cnt = read_paths(infile, it, &pathid_map)) < 0) {
    fprintf(stderr, "ERROR: Could not read path table\n");
    goto err;
  }

  /* pfxs */
  if (read_pfxs(infile, it, pfx_cb, pfx_peer_cb, peerid_map, peerid_map_cnt,
                pathid_map, pathid_map_cnt) != 0) {
    fprintf(stderr, "ERROR: Could not read prefixes\n");
    goto err;
  }

  if (check_magic(infile, VIEW_END_MAGIC) == 0) {
    fprintf(stderr, "ERROR: Missing end-of-view magic number\n");
  }

  if (it != NULL) {
    bgpview_iter_destroy(it);
  }

  free(peerid_map);

  /* valid view */
  return 1;

err:
  if (it != NULL) {
    bgpview_iter_destroy(it);
  }
  free(peerid_map);
  return -1;
}

int bgpview_io_file_print(iow_t *outfile, bgpview_t *view)
{
  bgpview_iter_t *it = NULL;

  uint32_t time;

  bgpstream_pfx_t *pfx;
  char pfx_str[INET6_ADDRSTRLEN + 3] = "";

  bgpstream_peer_sig_t *ps;

  char peer_str[INET6_ADDRSTRLEN] = "";

  char path_str[4096] = "";
  bgpstream_as_path_t *path = NULL;

  bgpstream_as_path_seg_t *orig_seg = NULL;
  char orig_str[4096] = "";

  if (view == NULL) {
    /* no-op */
    return 0;
  }

  time = bgpview_get_time(view);

  if ((it = bgpview_iter_create(view)) == NULL) {
    goto err;
  }

  wandio_printf(outfile, "# View %" PRIu32 "\n"
                         "# IPv4 Prefixes: %d\n"
                         "# IPv6 Prefixes: %d\n",
                time, bgpview_v4pfx_cnt(view, BGPVIEW_FIELD_ACTIVE),
                bgpview_v6pfx_cnt(view, BGPVIEW_FIELD_ACTIVE));

  for (bgpview_iter_first_pfx(it, 0, BGPVIEW_FIELD_ACTIVE);
       bgpview_iter_has_more_pfx(it); bgpview_iter_next_pfx(it)) {
    pfx = bgpview_iter_pfx_get_pfx(it);
    bgpstream_pfx_snprintf(pfx_str, INET6_ADDRSTRLEN + 3, pfx);

    /*
    fprintf(stdout, "  %s (%d peers)\n",
            pfx_str,
            bgpview_iter_pfx_get_peer_cnt(it,
                                                  BGPVIEW_FIELD_ACTIVE));
    */

    for (bgpview_iter_pfx_first_peer(it, BGPVIEW_FIELD_ACTIVE);
         bgpview_iter_pfx_has_more_peer(it); bgpview_iter_pfx_next_peer(it)) {
      ps = bgpview_iter_peer_get_sig(it);
      bgpstream_addr_ntop(peer_str, INET6_ADDRSTRLEN, &ps->peer_ip_addr);

      path = bgpview_iter_pfx_peer_get_as_path(it);
      orig_seg = bgpstream_as_path_get_origin_seg(path);

      bgpstream_as_path_seg_snprintf(orig_str, 4096, orig_seg);

      bgpstream_as_path_snprintf(path_str, 4096, path);
      bgpstream_as_path_destroy(path);

      wandio_printf(outfile, "%" PRIu32 "|" /* time */
                             "%s|"          /* prefix */
                             "%s|"          /* collector */
                             "%" PRIu32 "|" /* peer ASN */
                             "%s|"          /* peer IP */
                             "%s|"          /* path */
                             "%s"           /* origin segment */
                             "\n",
                    time, pfx_str, ps->collector_str, ps->peer_asnumber,
                    peer_str, path_str, orig_str);
    }
  }

  bgpview_iter_destroy(it);

  return 0;

err:
  return -1;
}
