/*
 * Copyright (c) 2022 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>

#include <zephyr/sys/byteorder.h>

#include "hal/ccm.h"
#include "hal/radio.h"
#include "hal/ticker.h"

#include "util/util.h"
#include "util/mem.h"
#include "util/memq.h"
#include "util/dbuf.h"

#include "pdu_df.h"
#include "pdu_vendor.h"
#include "pdu.h"

#include "lll.h"
#include "lll_vendor.h"
#include "lll_clock.h"
#include "lll_chan.h"
#include "lll_df_types.h"
#include "lll_conn.h"
#include "lll_conn_iso.h"
#include "lll_central_iso.h"

#include "lll_iso_tx.h"

#include "lll_internal.h"
#include "lll_tim_internal.h"

#include "ll_feat.h"

#include "hal/debug.h"

static int init_reset(void);
static int prepare_cb(struct lll_prepare_param *p);
static void isr_tx(void *param);
static void isr_rx(void *param);
static void isr_prepare_subevent(void *param);
static void isr_done(void *param);

static uint8_t next_chan_use;
static uint16_t data_chan_id;
static uint16_t data_chan_prn_s;
static uint16_t data_chan_remap_idx;
static uint32_t trx_performed_bitmask;
static uint8_t se_curr;
static uint8_t bn_tx;
static uint8_t bn_rx;

#if defined(CONFIG_BT_CTLR_LE_ENC)
static uint8_t mic_state;
#endif /* CONFIG_BT_CTLR_LE_ENC */

int lll_central_iso_init(void)
{
	int err;

	err = init_reset();
	if (err) {
		return err;
	}

	return 0;
}

int lll_central_iso_reset(void)
{
	int err;

	err = init_reset();
	if (err) {
		return err;
	}

	return 0;
}

void lll_central_iso_prepare(void *param)
{
	int err;

	/* Initiate HF clock start up */
	err = lll_hfclock_on();
	LL_ASSERT(err >= 0);

	/* Invoke common pipeline handling of prepare */
	err = lll_prepare(lll_is_abort_cb, lll_abort_cb, prepare_cb, 0U, param);
	LL_ASSERT(!err || err == -EINPROGRESS);
}

static int init_reset(void)
{
	return 0;
}

static int prepare_cb(struct lll_prepare_param *p)
{
	struct lll_conn_iso_group *cig_lll = p->param;
	struct lll_conn_iso_stream *cis_lll;
	struct lll_conn *conn_lll;
	uint32_t ticks_at_event;
	uint32_t ticks_at_start;
	struct pdu_cis *pdu_tx;
	uint16_t event_counter;
	uint64_t payload_count;
	uint8_t data_chan_use;
	struct ull_hdr *ull;
	uint32_t remainder;
	uint32_t start_us;
	uint8_t phy;

	DEBUG_RADIO_START_A(1);

	/* Reset global static variables */
	trx_performed_bitmask = 0U;
#if defined(CONFIG_BT_CTLR_LE_ENC)
	mic_state = LLL_CONN_MIC_NONE;
#endif /* CONFIG_BT_CTLR_LE_ENC */

	/* Get the first CIS */
	cis_lll = ull_conn_iso_lll_stream_get_by_group(cig_lll, NULL);

	/* Get reference to ACL context */
	conn_lll = ull_conn_lll_get(cis_lll->acl_handle);

	/* Event counter value,  0-15 bit of cisEventCounter */
	event_counter = cis_lll->event_count;

	/* Calculate the radio channel to use for ISO event */
	data_chan_id = lll_chan_id(cis_lll->access_addr);
	data_chan_use = lll_chan_iso_event(event_counter, data_chan_id,
					   conn_lll->data_chan_map,
					   conn_lll->data_chan_count,
					   &data_chan_prn_s,
					   &data_chan_remap_idx);

	se_curr = 1U;
	bn_tx = 1U;
	bn_rx = 1U;

	/* Start setting up of Radio h/w */
	radio_reset();

#if defined(CONFIG_BT_CTLR_TX_PWR_DYNAMIC_CONTROL)
	radio_tx_power_set(cis_lll->tx_pwr_lvl);
#else
	radio_tx_power_set(RADIO_TXP_DEFAULT);
#endif

	phy = cis_lll->tx.phy;
	radio_phy_set(phy, cis_lll->tx.phy_flags);
	radio_aa_set(cis_lll->access_addr);
	radio_crc_configure(PDU_CRC_POLYNOMIAL,
			    sys_get_le24(conn_lll->crc_init));
	lll_chan_set(data_chan_use);

	/* Get ISO data PDU */
	if (bn_tx > cis_lll->tx.bn) {
		payload_count = 0U;

		cis_lll->empty = 1U;

		pdu_tx = radio_pkt_empty_get();
		pdu_tx->ll_id = PDU_CIS_LLID_START_CONTINUE;
		pdu_tx->nesn = cis_lll->nesn;
		pdu_tx->sn = 0U; /* reserved RFU for NULL PDU */
		pdu_tx->cie = (bn_rx > cis_lll->rx.bn);
		pdu_tx->npi = 1U;
		pdu_tx->len = 0U;
	} else {
		struct node_tx_iso *node_tx;
		memq_link_t *link;

		payload_count = cis_lll->event_count * cis_lll->tx.bn;

		do {
			link = memq_peek(cis_lll->memq_tx.head,
					 cis_lll->memq_tx.tail,
					 (void **)&node_tx);
			if (link) {
				if (node_tx->payload_count < payload_count) {
					memq_dequeue(cis_lll->memq_tx.tail,
						     &cis_lll->memq_tx.head,
						     NULL);

					node_tx->next = link;
					ull_iso_lll_ack_enqueue(cis_lll->handle,
								node_tx);
				} else if (node_tx->payload_count >=
					   (payload_count + cis_lll->tx.bn)) {
					link = NULL;
				} else {
					if (node_tx->payload_count !=
					    payload_count) {
						link = NULL;
					}

					break;
				}
			}
		} while (link);

		if (!link) {
			cis_lll->empty = 1U;

			pdu_tx = radio_pkt_empty_get();
			pdu_tx->ll_id = PDU_CIS_LLID_START_CONTINUE;
			pdu_tx->nesn = cis_lll->nesn;
			pdu_tx->cie = (bn_tx > cis_lll->tx.bn) &&
				      (bn_rx > cis_lll->rx.bn);
			pdu_tx->len = 0U;
			if (bn_tx > cis_lll->tx.bn) {
				pdu_tx->sn = 0U; /* reserved RFU for NULL PDU */
				pdu_tx->npi = 1U;
			} else {
				pdu_tx->sn = cis_lll->sn;
				pdu_tx->npi = 0U;
			}
		} else {
			cis_lll->empty = 0U;

			pdu_tx = (void *)node_tx->pdu;
			pdu_tx->nesn = cis_lll->nesn;
			pdu_tx->sn = cis_lll->sn;
			pdu_tx->cie = 0U;
			pdu_tx->npi = 0U;
		}
	}

	/* Initialize reserve bit */
	pdu_tx->rfu0 = 0U;
	pdu_tx->rfu1 = 0U;

	/* Encryption */
	if (false) {

#if defined(CONFIG_BT_CTLR_LE_ENC)
	} else if (pdu_tx->len && conn_lll->enc_tx) {
		uint8_t pkt_flags;

		cis_lll->tx.ccm.counter = payload_count;

		pkt_flags = RADIO_PKT_CONF_FLAGS(RADIO_PKT_CONF_PDU_TYPE_DC,
						 phy,
						 RADIO_PKT_CONF_CTE_DISABLED);
		radio_pkt_configure(RADIO_PKT_CONF_LENGTH_8BIT,
				    (cis_lll->tx.max_pdu + PDU_MIC_SIZE),
				    pkt_flags);
		radio_pkt_tx_set(radio_ccm_tx_pkt_set(&cis_lll->tx.ccm,
						      pdu_tx));
#endif /* CONFIG_BT_CTLR_LE_ENC */

	} else {
		uint8_t pkt_flags;

		pkt_flags = RADIO_PKT_CONF_FLAGS(RADIO_PKT_CONF_PDU_TYPE_DC,
						 phy,
						 RADIO_PKT_CONF_CTE_DISABLED);
		radio_pkt_configure(RADIO_PKT_CONF_LENGTH_8BIT,
				    cis_lll->tx.max_pdu, pkt_flags);
		radio_pkt_tx_set(pdu_tx);
	}

	radio_isr_set(isr_tx, cis_lll);

	radio_tmr_tifs_set(EVENT_IFS_US);

#if defined(CONFIG_BT_CTLR_PHY)
	radio_switch_complete_and_rx(cis_lll->rx.phy);
#else /* !CONFIG_BT_CTLR_PHY */
	radio_switch_complete_and_rx(0U);
#endif /* !CONFIG_BT_CTLR_PHY */

	ticks_at_event = p->ticks_at_expire;
	ull = HDR_LLL2ULL(cig_lll);
	ticks_at_event += lll_event_offset_get(ull);

	ticks_at_start = ticks_at_event;
	ticks_at_start += HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US);

	remainder = p->remainder;
	start_us = radio_tmr_start(1U, ticks_at_start, remainder);

	/* Save radio ready timestamp, use it to schedule next subevent */
	radio_tmr_ready_save(start_us);

	/* capture end of Tx-ed PDU, used to calculate HCTO. */
	radio_tmr_end_capture();

#if defined(HAL_RADIO_GPIO_HAVE_PA_PIN)
	radio_gpio_pa_setup();

#if defined(CONFIG_BT_CTLR_PHY)
	radio_gpio_pa_lna_enable(start_us +
				 radio_tx_ready_delay_get(phy, PHY_FLAGS_S8) -
				 HAL_RADIO_GPIO_PA_OFFSET);
#else /* !CONFIG_BT_CTLR_PHY */
	radio_gpio_pa_lna_enable(start_us +
				 radio_tx_ready_delay_get(0U, 0U) -
				 HAL_RADIO_GPIO_PA_OFFSET);
#endif /* !CONFIG_BT_CTLR_PHY */
#else /* !HAL_RADIO_GPIO_HAVE_PA_PIN */
	ARG_UNUSED(start_us);
#endif /* !HAL_RADIO_GPIO_HAVE_PA_PIN */

	if (false) {

#if defined(CONFIG_BT_CTLR_XTAL_ADVANCED) && \
	(EVENT_OVERHEAD_PREEMPT_US <= EVENT_OVERHEAD_PREEMPT_MIN_US)
	/* check if preempt to start has changed */
	} else if (lll_preempt_calc(ull,
				    (TICKER_ID_CONN_ISO_BASE + cig_lll->handle),
				    ticks_at_event)) {
		radio_isr_set(lll_isr_abort, cig_lll);
		radio_disable();

		return -ECANCELED;
#endif /* CONFIG_BT_CTLR_XTAL_ADVANCED */

	} else {
		uint32_t ret;

		ret = lll_prepare_done(cig_lll);
		LL_ASSERT(!ret);
	}

	DEBUG_RADIO_START_A(1);

	return 0;
}

static void isr_tx(void *param)
{
	struct lll_conn_iso_stream *cis_lll;
	struct node_rx_pdu *node_rx;
	struct lll_conn *conn_lll;
	uint32_t hcto;

	/* Clear radio tx status and events */
	lll_isr_tx_status_reset();

	/* Close subevent, one tx-rx chain */
	radio_switch_complete_and_disable();

	/* Get reference to CIS LLL context */
	cis_lll = param;

	/* Get reference to ACL context */
	conn_lll = ull_conn_lll_get(cis_lll->acl_handle);

	/* Acquire rx node for reception */
	node_rx = ull_iso_pdu_rx_alloc_peek(1U);
	LL_ASSERT(node_rx);

	/* Encryption */
	if (false) {

#if defined(CONFIG_BT_CTLR_LE_ENC)
	} else if (conn_lll->enc_rx) {
		uint64_t payload_count;
		uint8_t pkt_flags;

		payload_count = (cis_lll->event_count * cis_lll->rx.bn) +
				(bn_rx - 1U);
		cis_lll->rx.ccm.counter = payload_count;

		pkt_flags = RADIO_PKT_CONF_FLAGS(RADIO_PKT_CONF_PDU_TYPE_DC,
						 cis_lll->rx.phy,
						 RADIO_PKT_CONF_CTE_DISABLED);
		radio_pkt_configure(RADIO_PKT_CONF_LENGTH_8BIT,
				    (cis_lll->rx.max_pdu + PDU_MIC_SIZE),
				    pkt_flags);
		radio_pkt_rx_set(radio_ccm_rx_pkt_set(&cis_lll->rx.ccm,
						      cis_lll->rx.phy,
						      node_rx->pdu));
#endif /* CONFIG_BT_CTLR_LE_ENC */

	} else {
		uint8_t pkt_flags;

		pkt_flags = RADIO_PKT_CONF_FLAGS(RADIO_PKT_CONF_PDU_TYPE_DC,
						 cis_lll->rx.phy,
						 RADIO_PKT_CONF_CTE_DISABLED);
		radio_pkt_configure(RADIO_PKT_CONF_LENGTH_8BIT,
				    cis_lll->rx.max_pdu, pkt_flags);
		radio_pkt_rx_set(node_rx->pdu);
	}

	/* assert if radio packet ptr is not set and radio started rx */
	LL_ASSERT(!radio_is_ready());

	/* +/- 2us active clock jitter, +1 us hcto compensation */
	hcto = radio_tmr_tifs_base_get() + EVENT_IFS_US +
	       (EVENT_CLOCK_JITTER_US << 1) + RANGE_DELAY_US +
	       HCTO_START_DELAY_US;
#if defined(CONFIG_BT_CTLR_PHY)
	hcto += radio_rx_chain_delay_get(cis_lll->rx.phy, PHY_FLAGS_S8);
	hcto += addr_us_get(cis_lll->rx.phy);
	hcto -= radio_tx_chain_delay_get(cis_lll->tx.phy,
					 cis_lll->tx.phy_flags);
#else /* !CONFIG_BT_CTLR_PHY */
	hcto += radio_rx_chain_delay_get(0U, 0U);
	hcto += addr_us_get(0U);
	hcto -= radio_tx_chain_delay_get(0U, 0U);
#endif /* !CONFIG_BT_CTLR_PHY */

	radio_tmr_hcto_configure(hcto);

#if defined(CONFIG_BT_CTLR_PROFILE_ISR) || \
	defined(HAL_RADIO_GPIO_HAVE_PA_PIN)
	radio_tmr_end_capture();
#endif /* CONFIG_BT_CTLR_PROFILE_ISR */

#if defined(HAL_RADIO_GPIO_HAVE_LNA_PIN)
	radio_gpio_lna_setup();

#if defined(CONFIG_BT_CTLR_PHY)
	radio_gpio_pa_lna_enable(radio_tmr_tifs_base_get() + EVENT_IFS_US -
				 (EVENT_CLOCK_JITTER_US << 1) -
				 radio_tx_chain_delay_get(cis_lll->tx.phy,
							  cis_lll->tx.phy_flags) -
				 HAL_RADIO_GPIO_LNA_OFFSET);
#else /* !CONFIG_BT_CTLR_PHY */
	radio_gpio_pa_lna_enable(radio_tmr_tifs_base_get() + EVENT_IFS_US -
				 (EVENT_CLOCK_JITTER_US << 1) -
				 radio_tx_chain_delay_get(0U, 0U) -
				 HAL_RADIO_GPIO_LNA_OFFSET);
#endif /* !CONFIG_BT_CTLR_PHY */
#endif /* HAL_RADIO_GPIO_HAVE_LNA_PIN */

	radio_isr_set(isr_rx, param);

	/* Schedule next subevent */
	if (se_curr < cis_lll->nse) {
		struct lll_conn *conn_lll;
		uint32_t subevent_us;
		uint32_t start_us;

		subevent_us = radio_tmr_ready_restore();
		subevent_us += cis_lll->sub_interval * se_curr;

		start_us = radio_tmr_start_us(1U, subevent_us);
		LL_ASSERT(start_us == (subevent_us + 1U));

		/* Get reference to ACL context */
		conn_lll = ull_conn_lll_get(cis_lll->acl_handle);

		/* Calculate the radio channel to use for next subevent */
		next_chan_use = lll_chan_iso_subevent(data_chan_id,
						      conn_lll->data_chan_map,
						      conn_lll->data_chan_count,
						      &data_chan_prn_s,
						      &data_chan_remap_idx);
	}
}

static void isr_rx(void *param)
{
	struct lll_conn_iso_stream *cis_lll;
	struct node_rx_pdu *node_rx;
	struct pdu_cis *pdu_rx;
	uint8_t trx_done;
	uint8_t crc_ok;
	uint8_t cie;

	/* Read radio status and events */
	trx_done = radio_is_done();
	if (trx_done) {
		crc_ok = radio_crc_is_valid();
	} else {
		crc_ok = 0U;
	}

	/* Clear radio status and events */
	lll_isr_rx_sub_status_reset();

	/* Initialize Close Isochronous Event */
	cie = 0U;

	/* Get reference to CIS LLL context */
	cis_lll = param;

	/* No Rx */
	if (!trx_done) {
		goto isr_rx_next_subevent;
	}

	/* FIXME: Do not call this for every event/subevent */
	ull_conn_iso_lll_cis_established(param);

	/* FIXME: set the bit corresponding to CIS index */
	trx_performed_bitmask = 1U;

	/* Get reference to received PDU */
	node_rx = ull_iso_pdu_rx_alloc_peek(1);
	LL_ASSERT(node_rx);

	pdu_rx = (void *)node_rx->pdu;

	if (crc_ok) {
		/* Tx ACK */
		if (pdu_rx->nesn != cis_lll->sn) {
			struct pdu_cis *pdu_tx;

			/* Increment sequence number */
			cis_lll->sn++;

			/* Get reference to PDU Tx */
			if (cis_lll->empty) {
				cis_lll->empty = 0U;

				pdu_tx = radio_pkt_empty_get();
			} else {
				struct node_tx_iso *node_tx;
				uint8_t payload_index;
				memq_link_t *link;

				payload_index = bn_tx - 1U;
				link = memq_peek_n(cis_lll->memq_tx.head,
						   cis_lll->memq_tx.tail,
						   payload_index,
						   (void **)&node_tx);
				if (link) {
					pdu_tx = (void *)node_tx->pdu;
				} else {
					pdu_tx = radio_pkt_empty_get();
				}
			}

#if defined(CONFIG_BT_CTLR_LE_ENC)
			/* Get reference to ACL context */
			struct lll_conn *conn_lll =
				ull_conn_lll_get(cis_lll->acl_handle);

			if (pdu_tx->len) {
				/* if encrypted increment tx counter */
				if (conn_lll->enc_tx) {
					cis_lll->tx.ccm.counter++;
				}
			}
#endif /* CONFIG_BT_CTLR_LE_ENC */

			/* Increment burst number */
			if (bn_tx <= cis_lll->tx.bn) {
				bn_tx++;
			}

			/* TODO: Tx Ack */
		}

		/* Rx receive */
		if (!pdu_rx->npi &&
		    (pdu_rx->sn == cis_lll->nesn) &&
		    ull_iso_pdu_rx_alloc_peek(2)) {
			struct node_rx_iso_meta *iso_meta;

			/* Increment next expected serial number */
			cis_lll->nesn++;

#if defined(CONFIG_BT_CTLR_LE_ENC)
			/* Get reference to ACL context */
			struct lll_conn *conn_lll =
				ull_conn_lll_get(cis_lll->acl_handle);

			/* If required, wait for CCM to finish
			 */
			if (pdu_rx->len && conn_lll->enc_rx) {
				uint32_t done;

				done = radio_ccm_is_done();
				LL_ASSERT(done);

				if (!radio_ccm_mic_is_valid()) {
					/* Record MIC invalid */
					mic_state = LLL_CONN_MIC_FAIL;

					goto isr_rx_done;
				}

				/* Increment counter */
				cis_lll->rx.ccm.counter++;

				/* Record MIC valid */
				mic_state = LLL_CONN_MIC_PASS;
			}
#endif /* CONFIG_BT_CTLR_LE_ENC */

			/* Enqueue Rx ISO PDU */
			node_rx->hdr.type = NODE_RX_TYPE_ISO_PDU;
			node_rx->hdr.handle = cis_lll->handle;
			iso_meta = &node_rx->hdr.rx_iso_meta;
			iso_meta->payload_number = (cis_lll->event_count *
						    cis_lll->rx.bn) +
						   (bn_rx - 1U);
			iso_meta->timestamp =
				HAL_TICKER_TICKS_TO_US(radio_tmr_start_get()) +
				radio_tmr_ready_restore();
			iso_meta->timestamp %=
				HAL_TICKER_TICKS_TO_US(BIT(HAL_TICKER_CNTR_MSBIT + 1U));
			iso_meta->status = 0U;

			ull_iso_pdu_rx_alloc();
			iso_rx_put(node_rx->hdr.link, node_rx);

#if !defined(CONFIG_BT_CTLR_LOW_LAT_ULL)
			iso_rx_sched();
#endif /* CONFIG_BT_CTLR_LOW_LAT_ULL */

			/* Increment burst number */
			if (bn_rx <= cis_lll->rx.bn) {
				bn_rx++;
			}
		}

		/* Close Isochronous Event */
		cie = cie || pdu_rx->cie;
	}

	/* Close Isochronous Event */
	cie = (cie || (bn_rx > cis_lll->rx.bn)) && (bn_tx > cis_lll->tx.bn);

isr_rx_next_subevent:
	if (cie || (se_curr == cis_lll->nse)) {
		goto isr_rx_done;
	}

	isr_prepare_subevent(param);

	return;

isr_rx_done:
	radio_isr_set(isr_done, param);
	radio_disable();
}

static void isr_prepare_subevent(void *param)
{
	struct lll_conn_iso_stream *cis_lll;
	struct pdu_cis *pdu_tx;
	uint64_t payload_count;
	uint8_t payload_index;
	uint32_t subevent_us;
	uint32_t start_us;

	/* Get reference to CIS LLL context */
	cis_lll = param;

	/* Get ISO data PDU */
	if (bn_tx > cis_lll->tx.bn) {
		payload_count = 0U;

		cis_lll->empty = 1U;

		pdu_tx = radio_pkt_empty_get();
		pdu_tx->ll_id = PDU_CIS_LLID_START_CONTINUE;
		pdu_tx->nesn = cis_lll->nesn;
		pdu_tx->sn = 0U; /* reserved RFU for NULL PDU */
		pdu_tx->cie = (bn_rx > cis_lll->rx.bn);
		pdu_tx->npi = 1U;
		pdu_tx->len = 0U;
	} else {
		struct node_tx_iso *node_tx;
		memq_link_t *link;

		payload_index = bn_tx - 1U;
		payload_count = cis_lll->event_count * cis_lll->tx.bn +
				payload_index;

		link = memq_peek_n(cis_lll->memq_tx.head, cis_lll->memq_tx.tail,
				   payload_index, (void **)&node_tx);
		if (!link || (node_tx->payload_count != payload_count)) {
			payload_index = 0U;
			do {
				link = memq_peek_n(cis_lll->memq_tx.head,
						   cis_lll->memq_tx.tail,
						   payload_index,
						   (void **)&node_tx);
				payload_index++;
			} while (link &&
				 (node_tx->payload_count < payload_count));
		}

		if (!link || (node_tx->payload_count != payload_count)) {
			cis_lll->empty = 1U;

			pdu_tx = radio_pkt_empty_get();
			pdu_tx->ll_id = PDU_CIS_LLID_START_CONTINUE;
			pdu_tx->nesn = cis_lll->nesn;
			pdu_tx->cie = (bn_tx > cis_lll->tx.bn) &&
				      (bn_rx > cis_lll->rx.bn);
			pdu_tx->len = 0U;
			if (bn_tx > cis_lll->tx.bn) {
				pdu_tx->sn = 0U; /* reserved RFU for NULL PDU */
				pdu_tx->npi = 1U;
			} else {
				pdu_tx->sn = cis_lll->sn;
				pdu_tx->npi = 0U;
			}
		} else {
			cis_lll->empty = 0U;

			pdu_tx = (void *)node_tx->pdu;
			pdu_tx->nesn = cis_lll->nesn;
			pdu_tx->sn = cis_lll->sn;
			pdu_tx->cie = 0U;
			pdu_tx->npi = 0U;
		}
	}

	/* Initialize reserve bit */
	pdu_tx->rfu0 = 0U;
	pdu_tx->rfu1 = 0U;

#if defined(CONFIG_BT_CTLR_LE_ENC)
	/* Get reference to ACL context */
	struct lll_conn *conn_lll = ull_conn_lll_get(cis_lll->acl_handle);
#endif /* CONFIG_BT_CTLR_LE_ENC */

	/* Encryption */
	if (false) {

#if defined(CONFIG_BT_CTLR_LE_ENC)
	} else if (pdu_tx->len && conn_lll->enc_tx) {
		uint8_t pkt_flags;

		cis_lll->tx.ccm.counter = payload_count;

		pkt_flags = RADIO_PKT_CONF_FLAGS(RADIO_PKT_CONF_PDU_TYPE_DC,
						 cis_lll->tx.phy,
						 RADIO_PKT_CONF_CTE_DISABLED);
		radio_pkt_configure(RADIO_PKT_CONF_LENGTH_8BIT,
				    (cis_lll->tx.max_pdu + PDU_MIC_SIZE), pkt_flags);
		radio_pkt_tx_set(radio_ccm_tx_pkt_set(&cis_lll->tx.ccm,
						      pdu_tx));
#endif /* CONFIG_BT_CTLR_LE_ENC */

	} else {
		uint8_t pkt_flags;

		pkt_flags = RADIO_PKT_CONF_FLAGS(RADIO_PKT_CONF_PDU_TYPE_DC,
						 cis_lll->tx.phy,
						 RADIO_PKT_CONF_CTE_DISABLED);
		radio_pkt_configure(RADIO_PKT_CONF_LENGTH_8BIT,
				    cis_lll->tx.max_pdu, pkt_flags);
		radio_pkt_tx_set(pdu_tx);
	}

	lll_chan_set(next_chan_use);

	subevent_us = radio_tmr_ready_restore();
	subevent_us += cis_lll->sub_interval * se_curr;

	radio_tmr_rx_disable();
	radio_tmr_tx_enable();

	/* Compensate for the 1 us added by radio_tmr_start_us() */
	start_us = subevent_us + 1U;

	radio_tmr_tifs_set(EVENT_IFS_US);

#if defined(CONFIG_BT_CTLR_PHY)
	radio_switch_complete_and_rx(cis_lll->rx.phy);
#else /* !CONFIG_BT_CTLR_PHY */
	radio_switch_complete_and_rx(0U);
#endif /* !CONFIG_BT_CTLR_PHY */

	/* capture end of Tx-ed PDU, used to calculate HCTO. */
	radio_tmr_end_capture();

#if defined(HAL_RADIO_GPIO_HAVE_PA_PIN)
	radio_gpio_pa_setup();

#if defined(CONFIG_BT_CTLR_PHY)
	radio_gpio_pa_lna_enable(start_us +
				 radio_tx_ready_delay_get(cis_lll->rx.phy,
							  PHY_FLAGS_S8) -
				 HAL_RADIO_GPIO_PA_OFFSET);
#else /* !CONFIG_BT_CTLR_PHY */
	radio_gpio_pa_lna_enable(start_us +
				 radio_tx_ready_delay_get(0U, 0U) -
				 HAL_RADIO_GPIO_PA_OFFSET);
#endif /* !CONFIG_BT_CTLR_PHY */
#else /* !HAL_RADIO_GPIO_HAVE_PA_PIN */
	ARG_UNUSED(start_us);
#endif /* !HAL_RADIO_GPIO_HAVE_PA_PIN */

	/* assert if radio packet ptr is not set and radio started tx */
	LL_ASSERT(!radio_is_ready());

	radio_isr_set(isr_tx, param);

	/* Next subevent */
	se_curr++;
}

static void isr_done(void *param)
{
	struct lll_conn_iso_stream *cis_lll;
	struct event_done_extra *e;
	uint8_t bn;

	lll_isr_status_reset();

	/* Get reference to CIS LLL context */
	cis_lll = param;

	/* Adjust sn when flushing Tx */
	/* FIXME: When Flush Timeout is implemented */
	if (bn_tx <= cis_lll->tx.bn) {
		cis_lll->sn += cis_lll->tx.bn + 1U - bn_tx;
	}

	/* Adjust nesn when flushing Rx */
	/* FIXME: When Flush Timeout is implemented */
	if (bn_rx <= cis_lll->rx.bn) {
		cis_lll->nesn += cis_lll->rx.bn + 1U - bn_rx;
	}

	/* Generate ISO Data Invalid Status */
	bn = bn_rx;
	while (bn <= cis_lll->rx.bn) {
		struct node_rx_iso_meta *iso_meta;
		struct node_rx_pdu *node_rx;

		node_rx = ull_iso_pdu_rx_alloc_peek(2U);
		if (!node_rx) {
			break;
		}

		node_rx->hdr.type = NODE_RX_TYPE_ISO_PDU;
		node_rx->hdr.handle = cis_lll->handle;
		iso_meta = &node_rx->hdr.rx_iso_meta;
		iso_meta->payload_number = (cis_lll->event_count *
					    cis_lll->rx.bn) + (bn - 1U);
		iso_meta->timestamp =
			HAL_TICKER_TICKS_TO_US(radio_tmr_start_get()) +
			radio_tmr_ready_restore();
		iso_meta->timestamp %=
			HAL_TICKER_TICKS_TO_US(BIT(HAL_TICKER_CNTR_MSBIT + 1U));
		iso_meta->status = 1U;

		ull_iso_pdu_rx_alloc();
		iso_rx_put(node_rx->hdr.link, node_rx);

		bn++;
	}

#if !defined(CONFIG_BT_CTLR_LOW_LAT_ULL)
	if (bn != bn_rx) {
		iso_rx_sched();
	}
#endif /* CONFIG_BT_CTLR_LOW_LAT_ULL */

	e = ull_event_done_extra_get();
	LL_ASSERT(e);

	e->type = EVENT_DONE_EXTRA_TYPE_CIS;
	e->trx_performed_bitmask = trx_performed_bitmask;
	e->crc_valid = 1U;

#if defined(CONFIG_BT_CTLR_LE_ENC)
	e->mic_state = mic_state;
#endif /* CONFIG_BT_CTLR_LE_ENC */

	lll_isr_cleanup(param);
}
