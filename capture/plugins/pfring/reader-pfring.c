/* reader-pfring.c  -- pfring instead of libpcap
 *
 *  Simple plugin that queries the wise service for
 *  ips, domains, email, and md5s which can use various
 *  services to return data.  It caches all the results.
 *
 * Copyright 2012-2016 AOL Inc. All rights reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this Software except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "moloch.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "pfring.h"
#include "pcap.h"

extern MolochConfig_t        config;
extern MolochPcapFileHdr_t   pcapFileHeader;

#define MAX_INTERFACES 10
LOCAL pfring                *rings[MAX_INTERFACES];
LOCAL struct bpf_program    *bpf_programs = 0;

/******************************************************************************/
int reader_pfring_stats(MolochReaderStats_t *stats)
{
    pfring_stat pfstats;

    stats->dropped = 0;
    stats->total = 0;

    int i;
    for (i = 0; i < MAX_INTERFACES && config.interface[i]; i++) {
        pfring_stats(rings[i], &pfstats);
        stats->dropped += pfstats.drop;
        stats->total += pfstats.recv;
    }
    return 0;
}
/******************************************************************************/
void reader_pfring_packet_cb(const struct pfring_pkthdr *h, const u_char *p, const u_char *UNUSED(user_bytes))
{
    if (unlikely(h->caplen != h->len)) {
        LOG("ERROR - Moloch requires full packet captures caplen: %d pktlen: %d", h->caplen, h->len);
        exit (0);
    }

    MolochPacket_t *packet = MOLOCH_TYPE_ALLOC0(MolochPacket_t);

    packet->pkt           = (u_char *)p;
    packet->ts            = h->ts;
    packet->pktlen        = h->len;

    moloch_packet(packet);
}
/******************************************************************************/
static void *reader_pfring_thread(void *ringv)
{
    pfring                *ring = ringv;

    pfring_enable_ring(ring);
    while (1) {
        int r = pfring_loop(ring, reader_pfring_packet_cb, NULL, -1);

        // Some kind of failure we quit
        if (unlikely(r <= 0)) {
            moloch_quit();
            ring = 0;
            break;
        }
    }
    return NULL;
}
/******************************************************************************/
int reader_pfring_should_filter(const MolochPacket_t *UNUSED(packet))
{
    int i;
    for (i = 0; i < config.dontSaveBPFsNum; i++) {
        if (bpf_filter(bpf_programs[i].bf_insns, packet->pkt, packet->pktlen, packet->pktlen)) {
            return i;
            break;
        }
    }
    return -1;
}
/******************************************************************************/
void reader_pfring_start() {
    int dlt_to_linktype(int dlt);

    pcapFileHeader.linktype = 1;
    pcapFileHeader.snaplen = MOLOCH_SNAPLEN;


    pcap_t *pcap = pcap_open_dead(pcapFileHeader.linktype, pcapFileHeader.snaplen);
    if (config.dontSaveBPFs) {
        int i;
        if (bpf_programs) {
            for (i = 0; i < config.dontSaveBPFsNum; i++) {
                pcap_freecode(&bpf_programs[i]);
            }
        } else {
            bpf_programs = malloc(config.dontSaveBPFsNum*sizeof(struct bpf_program));
        }
        for (i = 0; i < config.dontSaveBPFsNum; i++) {
            if (pcap_compile(pcap, &bpf_programs[i], config.dontSaveBPFs[i], 0, PCAP_NETMASK_UNKNOWN) == -1) {
                LOG("ERROR - Couldn't compile filter: '%s' with %s", config.dontSaveBPFs[i], pcap_geterr(pcap));
                exit(1);
            }
        }
        moloch_reader_should_filter = reader_pfring_should_filter;
    }
    pcap_close(pcap);

    int i;
    for (i = 0; i < MAX_INTERFACES && config.interface[i]; i++) {
        char name[100];
        snprintf(name, sizeof(name), "moloch-pfring%d", i);
        g_thread_new(name, &reader_pfring_thread, rings[i]);
    }
}
/******************************************************************************/
void reader_pfring_stop() 
{

    int i;
    for (i = 0; i < MAX_INTERFACES && config.interface[i]; i++) {
        if (rings[i])
            pfring_breakloop(rings[i]);
    }
}
/******************************************************************************/
void reader_pfring_init(char *UNUSED(name))
{
    int flags = PF_RING_PROMISC | PF_RING_TIMESTAMP;
    int clusterId = moloch_config_int(NULL, "pfringClusterId", 0, 0, 255);

    int i;
    for (i = 0; i < MAX_INTERFACES && config.interface[i]; i++) {
        rings[i] = pfring_open(config.interface[i], MOLOCH_SNAPLEN, flags);

        if (config.bpf) {
            int err = pfring_set_bpf_filter(rings[i], config.bpf);

            if (err < 0) {
                LOG("pfring set filter error %d  for  %s", err, config.bpf);
                exit (1);
            }
        }


        if (!rings[i]) {
            LOG("pfring open failed! - %s", config.interface[i]);
            exit(1);
        }

        pfring_set_cluster(rings[i], clusterId, cluster_per_flow_5_tuple);
        pfring_set_application_name(rings[i], "moloch-capture");
        pfring_set_poll_watermark(rings[i], 64);
        pfring_enable_rss_rehash(rings[i]);
    }

    moloch_reader_start         = reader_pfring_start;
    moloch_reader_stop          = reader_pfring_stop;
    moloch_reader_stats         = reader_pfring_stats;
}
/******************************************************************************/
void moloch_plugin_init()
{
    moloch_readers_add("pfring", reader_pfring_init);
}
