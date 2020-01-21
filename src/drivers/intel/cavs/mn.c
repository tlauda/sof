// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2020 Intel Corporation. All rights reserved.
//
// Author: Janusz Jankowski <janusz.jankowski@linux.intel.com>

#include <sof/drivers/mn.h>
#include <sof/drivers/ssp.h>
#include <sof/lib/memory.h>
#include <sof/lib/shim.h>
#include <sof/math/numbers.h>
#include <sof/sof.h>
#include <sof/spinlock.h>
#include <sof/trace/trace.h>
#include <stdbool.h>
#include <stdint.h>

/* tracing */
#define trace_mn(__e, ...) \
	trace_event(TRACE_CLASS_MN, __e, ##__VA_ARGS__)
#define trace_mn_error(__e, ...) \
	trace_error(TRACE_CLASS_MN, __e, ##__VA_ARGS__)
#define tracev_mn(__e, ...) \
	tracev_event(TRACE_CLASS_MN, __e, ##__VA_ARGS__)

/** \brief BCLKs can be driven by multiple sources - M/N or XTAL directly.
 *	   Even in the case of M/N, the actual clock source can be XTAL,
 *	   Audio cardinal clock (24.576) or 96 MHz PLL.
 *	   The MN block is not really the source of clocks, but rather
 *	   an intermediate component.
 *	   Input for source is shared by all outputs coming from that source
 *	   and once it's in use, it can be adjusted only with dividers.
 *	   In order to change input, the source should not be in use, that's why
 *	   it's necessary to keep track of BCLKs sources to know when it's safe
 *	   to change shared input clock.
 */
enum bclk_source {
	MN_BCLK_SOURCE_NONE = 0, /**< port is not using any clock */
	MN_BCLK_SOURCE_MN, /**< port is using clock driven by M/N */
	MN_BCLK_SOURCE_XTAL, /**< port is using XTAL directly */
};

struct mn {
	/**< keep track of which MCLKs are in use to know when it's safe to
	 * change shared clock
	 */
	bool mclk_sources_used[DAI_NUM_SSP_MCLK];
	int mclk_source_clock;

	enum bclk_source bclk_sources[(DAI_NUM_SSP_BASE + DAI_NUM_SSP_EXT)];
	int bclk_source_mn_clock;

	spinlock_t lock; /**< lock mechanism */
};

static SHARED_DATA struct mn mn;

void mn_init(struct sof *sof)
{
	int i;

	sof->mn = cache_to_uncache(&mn);

	for (i = 0; i < ARRAY_SIZE(sof->mn->bclk_sources); i++)
		sof->mn->bclk_sources[i] = MN_BCLK_SOURCE_NONE;

	spinlock_init(&sof->mn->lock);

	platform_shared_commit(sof->mn, sizeof(*sof->mn));
}

/**
 * \brief Checks if given clock is used as source for any MCLK.
 * \param[in] clk_src MCLK source.
 * \return true if any port use given clock source, false otherwise.
 */
static inline bool is_mclk_source_in_use(void)
{
	struct mn *mn = mn_get();
	bool ret = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(mn->mclk_sources_used); i++) {
		if (mn->mclk_sources_used[i]) {
			ret = true;
			break;
		}
	}

	platform_shared_commit(mn, sizeof(*mn));

	return false;
}

/**
 * \brief Configures source clock for MCLK.
 *	  All MCLKs share the same source, so it should be changed
 *	  only if there are no other ports using it already.
 * \param[in] mclk_rate master clock frequency.
 * \return 0 on success, error code otherwise.
 */
static inline int setup_initial_mclk_source(uint32_t mclk_rate)
{
	struct mn *mn = mn_get();
	int i;
	int clk_index = -1;
	uint32_t mdivc = mn_reg_read(MN_MDIVCTRL);
	int ret = 0;

	/* searching the smallest possible mclk source */
	for (i = MAX_SSP_FREQ_INDEX; i >= 0; i--) {
		if (mclk_rate > ssp_freq[i].freq)
			break;

		if (ssp_freq[i].freq % mclk_rate == 0)
			clk_index = i;
	}

	if (clk_index < 0) {
		trace_mn_error("error: MCLK %d, no valid source",
			       mclk_rate);
		ret = -EINVAL;
		goto out;
	}

	mn->mclk_source_clock = clk_index;

	/* enable MCLK divider */
	mdivc |= MN_MDIVCTRL_M_DIV_ENABLE;

	/* select source clock */
	mdivc |= MCDSS(ssp_freq_sources[clk_index]);

	mn_reg_write(MN_MDIVCTRL, mdivc);

out:
	platform_shared_commit(mn, sizeof(*mn));

	return ret;
}

/**
 * \brief Checks if requested MCLK can be achieved with current source.
 * \param[in] mclk_rate master clock frequency.
 * \return 0 on success, error code otherwise.
 */
static inline int check_current_mclk_source(uint32_t mclk_rate)
{
	struct mn *mn = mn_get();
	int ret = 0;

	if (ssp_freq[mn->mclk_source_clock].freq % mclk_rate != 0) {
		trace_mn_error("error: MCLK %d, no valid configuration for already selected source = %d",
			       mclk_rate, mn->mclk_source_clock);
		ret = -EINVAL;
	}

	platform_shared_commit(mn, sizeof(*mn));

	return ret;
}

/**
 * \brief Sets MCLK divider to given value.
 * \param[in] mclk_id ID of MCLK.
 * \param[in] mdivr_val divider value.
 * \return 0 on success, error code otherwise.
 */
static inline int set_mclk_divider(uint16_t mclk_id, uint32_t mdivr_val)
{
	uint32_t mdivr;

	switch (mdivr_val) {
	case 1:
		mdivr = 0x00000fff; /* bypass divider for MCLK */
		break;
	case 2:
		mdivr = 0x0; /* 1/2 */
		break;
	case 4:
		mdivr = 0x2; /* 1/4 */
		break;
	case 8:
		mdivr = 0x6; /* 1/8 */
		break;
	default:
		trace_mn_error("error: invalid mdivr_val %d", mdivr_val);
		return -EINVAL;
	}

	mn_reg_write(MN_MDIVR(mclk_id), mdivr);
	return 0;
}

int mn_set_mclk(uint16_t mclk_id, uint32_t mclk_rate)
{
	struct mn *mn = mn_get();
	int ret = 0;

	if (mclk_id >= DAI_NUM_SSP_MCLK) {
		trace_mn_error("error: mclk ID (%d) >= %d",
			       mclk_id, DAI_NUM_SSP_MCLK);
		return -EINVAL;
	}

	spin_lock(&mn->lock);

	mn->mclk_sources_used[mclk_id] = false;

	if (is_mclk_source_in_use())
		ret = check_current_mclk_source(mclk_rate);
	else
		ret = setup_initial_mclk_source(mclk_rate);

	if (ret < 0)
		goto out;

	mn->mclk_sources_used[mclk_id] = true;

	ret = set_mclk_divider(mclk_id,
			       ssp_freq[mn->mclk_source_clock].freq /
			       mclk_rate);

out:
	platform_shared_commit(mn, sizeof(*mn));

	spin_unlock(&mn->lock);

	return ret;
}

void mn_release_mclk(uint32_t mclk_id)
{
	struct mn *mn = mn_get();

	spin_lock(&mn->lock);
	mn->mclk_sources_used[mclk_id] = false;
	platform_shared_commit(mn, sizeof(*mn));
	spin_unlock(&mn->lock);
}

/**
 * \brief Finds valid M/(N * SCR) values for given frequencies.
 * \param[in] freq SSP clock frequency.
 * \param[in] bclk Bit clock frequency.
 * \param[out] out_scr_div SCR divisor.
 * \param[out] out_m M value of M/N divider.
 * \param[out] out_n N value of M/N divider.
 * \return true if found suitable values, false otherwise.
 */
static bool find_mn(uint32_t freq, uint32_t bclk,
		    uint32_t *out_scr_div, uint32_t *out_m, uint32_t *out_n)
{
	uint32_t m, n, mn_div;
	uint32_t scr_div = freq / bclk;

	/* check if just SCR is enough */
	if (freq % bclk == 0 && scr_div < (SSCR0_SCR_MASK >> 8) + 1) {
		*out_scr_div = scr_div;
		*out_m = 1;
		*out_n = 1;

		return true;
	}

	/* M/(N * scr_div) has to be less than 1/2 */
	if ((bclk * 2) >= freq)
		return false;

	/* odd SCR gives lower duty cycle */
	if (scr_div > 1 && scr_div % 2 != 0)
		--scr_div;

	/* clamp to valid SCR range */
	scr_div = MIN(scr_div, (SSCR0_SCR_MASK >> 8) + 1);

	/* find highest even divisor */
	while (scr_div > 1 && freq % scr_div != 0)
		scr_div -= 2;

	/* compute M/N with smallest dividend and divisor */
	mn_div = gcd(bclk, freq / scr_div);

	m = bclk / mn_div;
	n = freq / scr_div / mn_div;

	/* M/N values can be up to 24 bits */
	if (n & (~0xffffff))
		return false;

	*out_scr_div = scr_div;
	*out_m = m;
	*out_n = n;

	return true;
}

/**
 * \brief Finds index of clock valid for given BCLK rate.
 *	  Clock that can use just SCR is preferred.
 *	  M/N other than 1/1 is used only if there are no other possibilities.
 * \param[in] bclk Bit clock frequency.
 * \param[out] scr_div SCR divisor.
 * \param[out] m M value of M/N divider.
 * \param[out] n N value of M/N divider.
 * \return index of suitable clock if could find it, -1 otherwise.
 */
static int find_bclk_source(uint32_t bclk,
			    uint32_t *scr_div, uint32_t *m, uint32_t *n)
{
	int i;

	/* searching the smallest possible bclk source */
	for (i = 0; i <= MAX_SSP_FREQ_INDEX; i++)
		if (ssp_freq[i].freq % bclk == 0) {
			*scr_div = ssp_freq[i].freq / bclk;
			return i;
		}

	/* check if we can get target BCLK with M/N */
	for (i = 0; i <= MAX_SSP_FREQ_INDEX; i++)
		if (find_mn(ssp_freq[i].freq, bclk,
			    scr_div, m, n))
			return i;

	return -1;
}

/**
 * \brief Checks if given clock is used as source for any BCLK.
 * \param[in] clk_src Bit clock source.
 * \return true if any port use given clock source, false otherwise.
 */
static inline bool is_bclk_source_in_use(enum bclk_source clk_src)
{
	struct mn *mn = mn_get();
	bool ret = false;
	int i;

	for (i = 0; i < ARRAY_SIZE(mn->bclk_sources); i++) {
		if (mn->bclk_sources[i] == clk_src) {
			ret = true;
			break;
		}
	}

	platform_shared_commit(mn, sizeof(*mn));

	return ret;
}

/**
 * \brief Configures M/N source clock for BCLK.
 *	  All ports that use M/N share the same source, so it should be changed
 *	  only if there are no other ports using M/N already.
 * \param[in] bclk Bit clock frequency.
 * \param[out] scr_div SCR divisor.
 * \param[out] m M value of M/N divider.
 * \param[out] n N value of M/N divider.
 * \return 0 on success, error code otherwise.
 */
static inline int setup_initial_bclk_mn_source(uint32_t bclk, uint32_t *scr_div,
					       uint32_t *m, uint32_t *n)
{
	struct mn *mn = mn_get();
	int clk_index = find_bclk_source(bclk, scr_div, m, n);

	if (clk_index < 0) {
		trace_mn_error("error: BCLK %d, no valid source", bclk);
		return -EINVAL;
	}

	mn->bclk_source_mn_clock = clk_index;

	mn_reg_write(MN_MDIVCTRL, (mn_reg_read(MN_MDIVCTRL) |
				   MNDSS(ssp_freq_sources[clk_index])));

	platform_shared_commit(mn, sizeof(*mn));

	return 0;
}

/**
 * \brief Finds valid M/(N * SCR) values for source clock that is already locked
 *	  because other ports use it.
 * \param[in] bclk Bit clock frequency.
 * \param[out] scr_div SCR divisor.
 * \param[out] m M value of M/N divider.
 * \param[out] n N value of M/N divider.
 * \return 0 on success, error code otherwise.
 */
static inline int setup_current_bclk_mn_source(uint32_t bclk, uint32_t *scr_div,
					       uint32_t *m, uint32_t *n)
{
	struct mn *mn = mn_get();
	int ret = 0;

	/* source for M/N is already set, no need to do it */
	if (find_mn(ssp_freq[mn->bclk_source_mn_clock].freq, bclk, scr_div, m,
		    n))
		goto out;

	trace_mn_error("error: BCLK %d, no valid configuration for already selected source = %d",
		       bclk, mn->bclk_source_mn_clock);
	ret = -EINVAL;

out:
	platform_shared_commit(mn, sizeof(*mn));

	return ret;
}

#if CAVS_VERSION >= CAVS_VERSION_2_0
static inline bool check_bclk_xtal_source(uint32_t bclk, bool mn_in_use,
					  uint32_t *scr_div)
{
	/* since cAVS 2.0 bypassing XTAL (ECS=0) is not supported */
	return false;
}
#else
/**
 * \brief Checks if XTAL source for BCLK should be used.
 *	  Before cAVS 2.0 BCLK could use XTAL directly (without M/N).
 *	  BCLK that use M/N = 1/1 or bypass XTAL is preferred.
 * \param[in] bclk Bit clock frequency.
 * \param[in] mn_in_use True if M/N source is already locked by another port.
 * \param[out] scr_div SCR divisor.
 * \return true if XTAL source should be used, false otherwise.
 */
static inline bool check_bclk_xtal_source(uint32_t bclk, bool mn_in_use,
					  uint32_t *scr_div)
{
	struct mn *mn = mn_get();
	bool ret = false;
	int i;

	for (i = 0; i <= MAX_SSP_FREQ_INDEX; i++) {
		if (ssp_freq[i].freq % bclk != 0)
			continue;

		if (ssp_freq_sources[i] == SSP_CLOCK_XTAL_OSCILLATOR) {
			/* XTAL turned out to be lowest source that can work
			 * just with SCR, so use it
			 */
			*scr_div = ssp_freq[i].freq / bclk;
			ret = true;
			break;
		}

		/* if M/N is already set up for desired clock,
		 * we can quit and let M/N logic handle it
		 */
		if (!mn_in_use || mn->bclk_source_mn_clock == i)
			break;
	}

	platform_shared_commit(mn, sizeof(*mn));

	return ret;
}
#endif

int mn_set_bclk(uint32_t dai_index, uint32_t bclk_rate,
		uint32_t *out_scr_div, bool *out_need_ecs)
{
	struct mn *mn = mn_get();
	uint32_t m = 1;
	uint32_t n = 1;
	int ret = 0;
	bool mn_in_use;

	spin_lock(&mn->lock);

	mn->bclk_sources[dai_index] = MN_BCLK_SOURCE_NONE;

	mn_in_use = is_bclk_source_in_use(MN_BCLK_SOURCE_MN);

	if (check_bclk_xtal_source(bclk_rate, mn_in_use, out_scr_div)) {
		mn->bclk_sources[dai_index] = MN_BCLK_SOURCE_XTAL;
		*out_need_ecs = false;
		goto out;
	}

	*out_need_ecs = true;

	if (mn_in_use)
		ret = setup_current_bclk_mn_source(bclk_rate,
						   out_scr_div, &m, &n);
	else
		ret = setup_initial_bclk_mn_source(bclk_rate,
						   out_scr_div, &m, &n);

	if (ret >= 0) {
		mn->bclk_sources[dai_index] = MN_BCLK_SOURCE_MN;

		mn_reg_write(MN_MDIV_M_VAL(dai_index), m);
		mn_reg_write(MN_MDIV_N_VAL(dai_index), n);
	}

out:
	platform_shared_commit(mn, sizeof(*mn));

	spin_unlock(&mn->lock);

	return ret;
}

void mn_release_bclk(uint32_t dai_index)
{
	struct mn *mn = mn_get();

	spin_lock(&mn->lock);
	mn->bclk_sources[dai_index] = MN_BCLK_SOURCE_NONE;
	platform_shared_commit(mn, sizeof(*mn));
	spin_unlock(&mn->lock);
}

void mn_reset_bclk_divider(uint32_t dai_index)
{
	struct mn *mn = mn_get();

	spin_lock(&mn->lock);
	mn_reg_write(MN_MDIV_M_VAL(dai_index), 1);
	mn_reg_write(MN_MDIV_N_VAL(dai_index), 1);
	spin_unlock(&mn->lock);
}
