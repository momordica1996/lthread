#include "../includes/aes_encrypt.h"
#include "../includes/nf_common.h"

typedef unsigned char BYTE;

/* AES encryption parameters */
BYTE key[1][32] = {
        {0x60,0x3d,0xeb,0x10,0x15,0xca,0x71,0xbe,0x2b,0x73,0xae,0xf0,0x85,0x7d,0x77,0x81,0x1f,0x35,0x2c,0x07,0x3b,0x61,0x08,0xd7,0x2d,0x98,0x10,0xa3,0x09,0x14,0xdf,0xf4}
};
BYTE iv[1][16] = {
        {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f}
};

/* init tenant state */
void
nf_aes_encrypt_init(struct nf_statistics *stats) {

    /* Initialise encryption engine. Key should be configurable. */
	aes_key_setup(key[0], stats->key_schedule, 256);
}

/* handle tenant packets */
uint16_t
nf_aes_encrypt_handler(struct rte_mbuf *pkt[], uint16_t num, struct nf_statistics *stats) {

    struct udp_hdr *udp;
    struct tcp_hdr *tcp;
	uint16_t i, num_out, plen, hlen;
    uint8_t *pkt_data, *eth;
    BYTE tmp_data[10000];

	num_out = 0;
	for (i = 0; i < num; i++) {
        /* Check if we have a valid UDP packet */
        udp = nf_pkt_udp_hdr(pkt[i]);
        if (udp != NULL) {
            /* Get at the payload */
            pkt_data = ((uint8_t *) udp) + sizeof(struct udp_hdr);
            /* Calculate length */
            eth = rte_pktmbuf_mtod(pkt[i], uint8_t *);
            hlen = pkt_data - eth;
            plen = pkt[i]->pkt_len - hlen;
            
            aes_encrypt_ctr(pkt_data, plen, tmp_data, stats->key_schedule, 256, iv[0]);
            num_out ++;
            continue;
        }
        /* Check if we have a valid TCP packet */
        tcp = nf_pkt_tcp_hdr(pkt[i]);
        if (tcp != NULL) {
            /* Get at the payload */
            pkt_data = ((uint8_t *) tcp) + sizeof(struct tcp_hdr);
            /* Calculate length */
            eth = rte_pktmbuf_mtod(pkt[i], uint8_t *);
            hlen = pkt_data - eth;
            plen = pkt[i]->pkt_len - hlen;

            aes_encrypt_ctr(pkt_data, plen, tmp_data, stats->key_schedule, 256, iv[0]);
            num_out ++;
        }
	}

	return num_out;
}

int
lthread_aes_encryt(void *dumy){

    uint16_t i, nf_id, nb_rx, nb_tx, cnt;
    struct nf_thread_info *tmp = (struct nf_thread_info *)dumy;
    struct nf_info *nf_info_local = &(nfs_info_data[tmp->nf_id]);
    struct rte_mbuf *pkts[BURST_SIZE];
    struct rte_ring *rq;
    struct rte_ring *tq;
    struct nf_statistics *statistics = nf_info_local->state;

    lthread_set_data((void *)nf_info_local);


    nf_id = nf_info_local->nf_id;
    rq = nf_info_local->rx_q;
    tq = nf_info_local->tx_q;

    printf("Core %d: Running NF thread %d\n", rte_lcore_id(), nf_id);
    nf_aes_encrypt_init(statistics);
    printf("finish init aes encryt\n");

    while (1){
        nb_rx = nf_ring_dequeue_burst(rq, pkts, BURST_SIZE, NULL);
        if (unlikely(nb_rx > 0)) {

            nf_aes_encrypt_handler(pkts, nb_rx, statistics);
            nb_tx = nf_ring_enqueue_burst(tq, pkts, nb_rx, NULL);
//            printf("aes encryt %d suc transfer %d pkts\n", nf_id, nb_tx);

        }else {
            continue;
        }
    }
    return 0;

}