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
#ifndef __ROUTINGTABLES_INT_H
#define __ROUTINGTABLES_INT_H

#include "bgpstream_elem.h"
#include "bgpview.h"
#include "khash.h"
#include "utils.h"
#include "routingtables.h"
#include "timeseries.h"
#include <stdint.h>

/** Default metric prefix */
#define RT_DEFAULT_METRIC_PFX "bgp"

/** Maximum string length for the metric prefix */
#define RT_METRIC_PFX_LEN 256

#if __STDC_VERSION__ >= 201100L
_Static_assert(sizeof(RT_DEFAULT_METRIC_PFX) <= RT_METRIC_PFX_LEN,
    "RT_DEFAULT_METRIC_PFX too long");
#endif

#if 0
/** The time granularity that is used to update the
 *  last wall time for a collector */
#define RT_COLLECTOR_WALL_UPDATE_FR 10000
#endif

/** If an information is inactive and not been seen in the
 *  last X hours, it definetely means that it has not been
 *  seen by any RIB in the last X hours, therefore, if inactive
 *  it can be removed from the view.  */
#define RT_DEPRECATED_INFO_INTERVAL (24 * 3600)

/* the prefix is not announced in the active state nor in the
 * under construction state */
#define RT_INITIAL_PFXSTATUS      0x00
#define RT_ANNOUNCED_PFXSTATUS    0x01
#define RT_UC_ANNOUNCED_PFXSTATUS 0x10

typedef enum {

  /** It is not possible to infer the state of
   *  the collector (e.g. initialization time,
   *  or corrupted data) */
  RT_COLLECTOR_STATE_UNKNOWN = 0,

  /** The collector is active */
  RT_COLLECTOR_STATE_UP = 1,

  /** The collector is inactive */
  RT_COLLECTOR_STATE_DOWN = 2,

} collector_state_t;

/** Information about the current status
 *  of a pfx-peer info */
typedef struct struct_perpfx_perpeer_info_t {
  // Note: the order of fields is designed to place fields at their natural
  // alignment even when the struct is packed.  (Misaligned fields may incur a
  // performance penalty and even cause memory errors on some architectures.)

  /** ID of the AS path observed in the current
   *  under construction RIB. */
  bgpstream_as_path_store_path_id_t uc_as_path_id;

  /** Difference between the current under
   *  construction RIB start time for the
   *  current peer and the last RIB message
   *  received for the prefix  */
  uint16_t bgp_time_uc_delta_ts;

  /** Last bgp time associated with the most
   *  recent operation involving the current
   *  prefix and the current peer  */
  uint32_t bgp_time_last_ts;

  /** Bitfield that indicates whether the
   *  prefix is currently announced by this peer
   *  in the active state and/or in the uc state */
  uint8_t pfx_status;

} __attribute__((packed)) perpfx_perpeer_info_t;

/** Indices of the peer metrics for a KP */
typedef struct peer_metric_idx {

  /* meta metrics */
  uint32_t status_idx;
  uint32_t inactive_v4_pfxs_idx;
  uint32_t inactive_v6_pfxs_idx;
  uint32_t rib_messages_cnt_idx;
  uint32_t pfx_announcements_cnt_idx;
  uint32_t pfx_withdrawals_cnt_idx;
  uint32_t state_messages_cnt_idx;
  uint32_t rib_positive_mismatches_cnt_idx;
  uint32_t rib_negative_mismatches_cnt_idx;

  /* data metrics */
  uint32_t active_v4_pfxs_idx;
  uint32_t active_v6_pfxs_idx;
  uint32_t announcing_origin_as_idx;
  uint32_t announced_v4_pfxs_idx;
  uint32_t withdrawn_v4_pfxs_idx;
  uint32_t announced_v6_pfxs_idx;
  uint32_t withdrawn_v6_pfxs_idx;

} __attribute__((packed)) peer_metric_idx_t;

/** A set that contains a unique set of origin segments */
KHASH_INIT(origin_segments, bgpstream_as_path_seg_t *, char, 0,
           bgpstream_as_path_seg_hash, bgpstream_as_path_seg_equal)
typedef khash_t(origin_segments) origin_segments_t;

/** Information about the current status
 *  of a peer */
typedef struct struct_perpeer_info_t {

  /** Graphite-safe collector string */
  char collector_str[BGPSTREAM_UTILS_STR_NAME_LEN];

  /** Graphite-safe peer string: peer_ASn.peer_IP */
  char peer_str[BGPSTREAM_UTILS_STR_NAME_LEN];

  /** BGP Finite State Machine of the current peer.
   *  If the peer is active, then its state
   *  is assumed BGPSTREAM_ELEM_PEERSTATE_ESTABLISHED,
   *  if the peer becomes inactive because of a state
   *  change then the bgp_fsm_state reflects the current
   *  fsm state, finally if the peer is inactive and no
   *  fsm state is known, then state is set to
   *  BGPSTREAM_ELEM_PEERSTATE_UNKNOWN */
  bgpstream_elem_peerstate_t bgp_fsm_state;

  /** first timestamp in the current reference RIB,
   *  or the time we set the current status
   *  (e.g. time of a peer established state) */
  uint32_t bgp_time_ref_rib_start;

  /** last timestamp in the current reference RIB,
   *  or the time we set the current status
   *  (e.g. time of a peer established state) */
  uint32_t bgp_time_ref_rib_end;

  /** first timestamp in the current under construction RIB,
   *  0 when the under construction process is off */
  uint32_t bgp_time_uc_rib_start;

  /** last timestamp in the current under construction RIB,
   *  0 when the under construction process is off */
  uint32_t bgp_time_uc_rib_end;

  /** last timestamp associated with information for the
   *  peer */
  uint32_t last_ts;

  /** Flag that checks whether the metrics have been generated
   *  or not (SOME PEERS (e.g. false peers generated by beacons
   *  or route servers never make to publication) */
  uint8_t metrics_generated;

  /** Indices of the peer metrics in the peer Key Package */
  peer_metric_idx_t kp_idxs;

  /** Number of rib messages received in the current
   * interval */
  uint32_t rib_messages_cnt;

  /** Number of announcements received in the current
   * interval */
  uint32_t pfx_announcements_cnt;

  /** Number of withdrawals received in the current
   * interval */
  uint32_t pfx_withdrawals_cnt;

  /** Number of state messages received in the current
   * interval */
  uint32_t state_messages_cnt;

  /** Set of ASns that announced at least one prefix
   *  in the current interval */
  origin_segments_t *announcing_ases;

  /** Set of prefixes that have been announced at
   *  least once in the current interval */
  bgpstream_pfx_set_t *announced_pfxs;

  /** Set of prefixes that have been withdrawn at
   *  least once in the current interval */
  bgpstream_pfx_set_t *withdrawn_pfxs;

  /** number of positive mismatches at rib end time
   *  i.e. number of active prefixes that are not
   *  observed in the new rib */
  uint32_t rib_positive_mismatches_cnt;

  /** number of negative mismatches at rib end time
   *  i.e. number of inactive prefixes that are
   *  instead observed in the new rib  */
  uint32_t rib_negative_mismatches_cnt;

} perpeer_info_t;

/** Indices of the collector metrics for a KP */
typedef struct collector_metric_idx {

  /* meta metrics */
  uint32_t processing_time_idx;
  uint32_t realtime_delay_idx;
  uint32_t valid_record_cnt_idx;
  uint32_t corrupted_record_cnt_idx;
  uint32_t empty_record_cnt_idx;

  uint32_t status_idx;
  uint32_t peers_cnt_idx;
  uint32_t active_peers_cnt_idx;
  uint32_t active_asns_cnt_idx;

} __attribute__((packed)) collector_metric_idx_t;

/** A set that contains a unique set of peer ids */
KHASH_INIT(peer_id_set, uint32_t, char, 0, kh_int_hash_func, kh_int_hash_equal)
typedef khash_t(peer_id_set) peer_id_set_t;

/** Information about the current status
 *  of a collector */
typedef struct struct_collector_t {

  /** graphite-safe collector string: project.collector */
  char collector_str[BGPSTREAM_UTILS_STR_NAME_LEN];

  /** unique set of peer ids that are associated
   *  peers providing information to the current */
  peer_id_set_t *collector_peerids;

  /** last time this collector was involved
   *  in bgp operations (bgp time) */
  uint32_t bgp_time_last;

#if 0 // not used
  /** last time this collector was involved
   *  in valid bgp operations (wall time) */
  uint32_t wall_time_last;
#endif

  /** dump time of the current reference RIB */
  uint32_t bgp_time_ref_rib_dump_time;

  /** start time of the current reference RIB */
  uint32_t bgp_time_ref_rib_start_time;

  /** dump time of the current under construction RIB,
   * or 0 if the under construction process is off */
  uint32_t bgp_time_uc_rib_dump_time;

  /** start time of the current under construction RIB */
  uint32_t bgp_time_uc_rib_start_time;

  /** Current status of the collector */
  collector_state_t state;

  /** Is the end of valid RIB due at the end of the interval?  */
  uint8_t eovrib_flag;

  /** Decide whether stats should be published */
  uint8_t publish_flag;

  /** Indices of the collector metrics in the collector Key Package */
  collector_metric_idx_t kp_idxs;

  /** number of active peers at the end of the interval */
  uint32_t active_peers_cnt;

  /** number of valid records received in the interval */
  uint32_t valid_record_cnt;

  /** number of valid records received in the interval */
  uint32_t corrupted_record_cnt;

  /** number of empty records received in the interval */
  uint32_t empty_record_cnt;

} collector_t;

/** A map that associates peer id to collectors*/
KHASH_INIT(peer_id_collector, uint32_t, collector_t *, 1, kh_int_hash_func,
           kh_int_hash_equal)
typedef khash_t(peer_id_collector) peer_id_collector_t;

/** A map that associates a collector_t with each collector name */
KHASH_INIT(collector_data, char *, collector_t *, 1, kh_str_hash_func,
           kh_str_hash_equal)
typedef khash_t(collector_data) collector_data_t;

/** Structure that manages all the routing
 *  tables that can be possibly built using
 *  the bgp stream in input */
struct struct_routingtables_t {

  /** Plugin name */
  char plugin_name[RT_METRIC_PFX_LEN];

  /** Table of peer id <-> peer signature
   * (shared with the view) */
  bgpstream_peer_sig_map_t *peersigns;

  /** Table of AS path ids <-> AS paths
   * (shared with the view) */
  bgpstream_as_path_store_t *pathstore;

  /** BGP view that contains the information associated
   *  with the active and inactive prefixes/peers/pfx-peer
   *  information. Every active field represents consistent
   *  states of the routing tables as seen by each peer
   *  of the each collector */
  bgpview_t *view;

  /** iterator associated with the view*/
  bgpview_iter_t *iter;

  /** Timeseries Key Package */
  timeseries_kp_t *kp;

  /** per collector information: name, peers and
   *  current state */
  collector_data_t *collectors;

  /* set of peers (and their collectors) for which we have to perform the end
   * of valid rib operations at the end of the interval (used only during
   * apply_end_of_valid_rib_operations(); stored here so its allocated memory
   * can be reused) */
  peer_id_collector_t *eorib_peers;

  /** unique set of active ASes per collector at the end of the interval
   * (used only during routingtables_dump_metrics(); stored here so its
   * allocated memory can be reused) */
  bgpstream_id_set_t *c_active_ases;

  /** Metric prefix */
  char metric_prefix[RT_METRIC_PFX_LEN];

  /** a borrowed pointer for timeseries */
  timeseries_t *timeseries;

  /** flag that tells whether metrics
   *  should be outputed or not */
  uint8_t metrics_output_on;

  /** beginning of the interval (bgp time) */
  uint32_t bgp_time_interval_start;

  /** end of the interval (bgp time) */
  uint32_t bgp_time_interval_end;

  /** last time (wall time) we received
   *  an interval_start signal */
  uint32_t wall_time_interval_start;
};

/** Read the view in the current routingtables instance and populate
 *  the metrics to be sent to the active timeseries back-ends
 *
 * @param rt            pointer to a routingtables instance to read
 * @param time_now      wall time at the end of the interval
 */
void routingtables_dump_metrics(routingtables_t *rt, uint32_t time_now);

/** Generate the metrics associated to a specific peer
 *
 * @param rt            pointer to a routingtables instance to read
 * @param p             pointer to a peer user pointer
 * @return 0 if the metrics were generated correctly, <0 if an error occurred.
 */
void peer_generate_metrics(routingtables_t *rt, perpeer_info_t *p);

/** Generate the metrics associated to a specific collector
 *
 * @param rt            pointer to a routingtables instance to read
 * @param c             pointer to a collector structure
 * @return 0 if the metrics were generated correctly, <0 if an error occurred.
 */
void collector_generate_metrics(routingtables_t *rt, collector_t *c);

#endif /* __ROUTINGTABLES_INT_H */
