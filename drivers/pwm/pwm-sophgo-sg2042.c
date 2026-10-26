// SPDX-License-Identifier: GPL-2.0
/*
 * Sophgo SG2042/SG2044/CV1800B PWM Controller Driver
 *
 * Copyright (C) 2024 Sophgo Technology Inc.
 * Copyright (C) 2024 Chen Wang <unicorn_wang@outlook.com>
 *
 * Limitations:
 * - After reset, the output of the SG2042 PWM channel is always high.
 *   The value of HLPERIOD/PERIOD is 0.
 * - When HLPERIOD or PERIOD is reconfigured, PWM will start to
 *   output waveforms with the new configuration after completing
 *   the running period.
 * - When PERIOD and HLPERIOD is set to 0, the SG2042 PWM wave output will
 *   be stopped and the output is pulled to high.
 * - SG2044 and CV1800B support both polarities, SG2042 only normal polarity.
 * - On CV1800B the output is driven to the inactive level when the channel is
 *   disabled (low for normal polarity, high for inverted polarity).
 * - On CV1800B a polarity change takes effect immediately, even while the
 *   channel is running.
 * - On CV1800B a 100% duty cycle is not representable. It is rounded down to
 *   one input clock cycle less than the period.
 * - On CV1800B the period must be at least 2 input clock cycles.
 * See the datasheets [1] and [2] for more details.
 * [1]:https://github.com/sophgo/sophgo-doc/tree/main/SG2042/TRM
 * [2]:https://github.com/sophgo/sophgo-doc/tree/main/SG200X/TRM
 */

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/math64.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pwm.h>
#include <linux/reset.h>

/*
 * Offset RegisterName
 * 0x0000 HLPERIOD0
 * 0x0004 PERIOD0
 * 0x0008 HLPERIOD1
 * 0x000C PERIOD1
 * 0x0010 HLPERIOD2
 * 0x0014 PERIOD2
 * 0x0018 HLPERIOD3
 * 0x001C PERIOD3
 * Four groups and every group is composed of HLPERIOD & PERIOD
 */
#define SG2042_PWM_HLPERIOD(chan) ((chan) * 8 + 0)
#define SG2042_PWM_PERIOD(chan) ((chan) * 8 + 4)

#define SG2044_PWM_POLARITY		0x40
#define SG2044_PWM_PWMSTART		0x44
#define SG2044_PWM_PWMUPDATE		0x4c
#define SG2044_PWM_OE			0xd0

#define SG2042_PWM_CHANNELNUM	4

/* CV1800B PERIOD/HLPERIOD are 30 bit wide and PERIOD must be > HLPERIOD. */
#define CV1800B_PWM_MAX_PERIOD	GENMASK(29, 0)
#define CV1800B_PWM_MIN_PERIOD	2

/**
 * struct sg2042_pwm_ddata - private driver data
 * @base:		base address of mapped PWM registers
 * @clk_rate_hz:	rate of base clock in HZ
 */
struct sg2042_pwm_ddata {
	void __iomem *base;
	unsigned long clk_rate_hz;
};

struct sg2042_chip_data {
	const struct pwm_ops ops;
};

/*
 * period_ticks: PERIOD
 * hlperiod_ticks: HLPERIOD
 */
static void pwm_sg2042_config(struct sg2042_pwm_ddata *ddata, unsigned int chan,
			      u32 period_ticks, u32 hlperiod_ticks)
{
	void __iomem *base = ddata->base;

	writel(period_ticks, base + SG2042_PWM_PERIOD(chan));
	writel(hlperiod_ticks, base + SG2042_PWM_HLPERIOD(chan));
}

static void pwm_sg2042_set_dutycycle(struct pwm_chip *chip, struct pwm_device *pwm,
				     const struct pwm_state *state)
{
	struct sg2042_pwm_ddata *ddata = pwmchip_get_drvdata(chip);
	u32 hlperiod_ticks;
	u32 period_ticks;

	/*
	 * Duration of High level (duty_cycle) = HLPERIOD x Period_of_input_clk
	 * Duration of One Cycle (period) = PERIOD x Period_of_input_clk
	 */
	period_ticks = min(mul_u64_u64_div_u64(ddata->clk_rate_hz, state->period, NSEC_PER_SEC), U32_MAX);
	hlperiod_ticks = min(mul_u64_u64_div_u64(ddata->clk_rate_hz, state->duty_cycle, NSEC_PER_SEC), U32_MAX);

	dev_dbg(pwmchip_parent(chip), "chan[%u]: ENABLE=%u, PERIOD=%u, HLPERIOD=%u, POLARITY=%u\n",
		pwm->hwpwm, state->enabled, period_ticks, hlperiod_ticks, state->polarity);

	pwm_sg2042_config(ddata, pwm->hwpwm, period_ticks, hlperiod_ticks);
}

static int pwm_sg2042_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	struct sg2042_pwm_ddata *ddata = pwmchip_get_drvdata(chip);

	if (state->polarity == PWM_POLARITY_INVERSED)
		return -EINVAL;

	if (!state->enabled) {
		pwm_sg2042_config(ddata, pwm->hwpwm, 0, 0);
		return 0;
	}

	pwm_sg2042_set_dutycycle(chip, pwm, state);

	return 0;
}

static int pwm_sg2042_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				struct pwm_state *state)
{
	struct sg2042_pwm_ddata *ddata = pwmchip_get_drvdata(chip);
	unsigned int chan = pwm->hwpwm;
	u32 hlperiod_ticks;
	u32 period_ticks;

	period_ticks = readl(ddata->base + SG2042_PWM_PERIOD(chan));
	hlperiod_ticks = readl(ddata->base + SG2042_PWM_HLPERIOD(chan));

	if (!period_ticks) {
		state->enabled = false;
		return 0;
	}

	if (hlperiod_ticks > period_ticks)
		hlperiod_ticks = period_ticks;

	state->enabled = true;
	state->period = DIV_ROUND_UP_ULL((u64)period_ticks * NSEC_PER_SEC, ddata->clk_rate_hz);
	state->duty_cycle = DIV_ROUND_UP_ULL((u64)hlperiod_ticks * NSEC_PER_SEC, ddata->clk_rate_hz);
	state->polarity = PWM_POLARITY_NORMAL;

	return 0;
}

static void pwm_sg2044_set_outputen(struct sg2042_pwm_ddata *ddata, struct pwm_device *pwm,
				    bool enabled)
{
	u32 pwmstart;

	pwmstart = readl(ddata->base + SG2044_PWM_PWMSTART);

	if (enabled)
		pwmstart |= BIT(pwm->hwpwm);
	else
		pwmstart &= ~BIT(pwm->hwpwm);

	writel(pwmstart, ddata->base + SG2044_PWM_PWMSTART);
}

static bool pwm_sg2044_get_outputen(struct sg2042_pwm_ddata *ddata, struct pwm_device *pwm)
{
	u32 pwmstart;

	pwmstart = readl(ddata->base + SG2044_PWM_PWMSTART);

	if (pwmstart & BIT(pwm->hwpwm))
		return true;

	return false;
}

static void pwm_sg2044_set_outputdir(struct sg2042_pwm_ddata *ddata, struct pwm_device *pwm,
				     bool enabled)
{
	u32 pwm_oe;

	pwm_oe = readl(ddata->base + SG2044_PWM_OE);

	if (enabled)
		pwm_oe |= BIT(pwm->hwpwm);
	else
		pwm_oe &= ~BIT(pwm->hwpwm);

	writel(pwm_oe, ddata->base + SG2044_PWM_OE);
}

static void pwm_sg2044_set_polarity(struct sg2042_pwm_ddata *ddata, struct pwm_device *pwm,
				    const struct pwm_state *state)
{
	u32 pwm_polarity;

	pwm_polarity = readl(ddata->base + SG2044_PWM_POLARITY);

	if (state->polarity == PWM_POLARITY_NORMAL)
		pwm_polarity &= ~BIT(pwm->hwpwm);
	else
		pwm_polarity |= BIT(pwm->hwpwm);

	writel(pwm_polarity, ddata->base + SG2044_PWM_POLARITY);
}

static int pwm_sg2044_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			    const struct pwm_state *state)
{
	struct sg2042_pwm_ddata *ddata = pwmchip_get_drvdata(chip);

	pwm_sg2044_set_polarity(ddata, pwm, state);

	pwm_sg2042_set_dutycycle(chip, pwm, state);

	/*
	 * re-enable PWMSTART to refresh the register period
	 */
	pwm_sg2044_set_outputen(ddata, pwm, false);

	if (!state->enabled)
		return 0;

	pwm_sg2044_set_outputdir(ddata, pwm, true);
	pwm_sg2044_set_outputen(ddata, pwm, true);

	return 0;
}

static int pwm_cv1800b_get_state(struct pwm_chip *chip, struct pwm_device *pwm,
				 struct pwm_state *state)
{
	struct sg2042_pwm_ddata *ddata = pwmchip_get_drvdata(chip);
	unsigned int chan = pwm->hwpwm;
	u32 hlperiod_ticks;
	u32 period_ticks;
	u32 pwm_polarity;

	/* PWMSTART is set to 1 while the channel is running. */
	state->enabled = pwm_sg2044_get_outputen(ddata, pwm);

	period_ticks = readl(ddata->base + SG2042_PWM_PERIOD(chan));
	hlperiod_ticks = readl(ddata->base + SG2042_PWM_HLPERIOD(chan));

	if (period_ticks && hlperiod_ticks >= period_ticks)
		hlperiod_ticks = period_ticks - 1;

	state->period = DIV_ROUND_UP_ULL((u64)period_ticks * NSEC_PER_SEC, ddata->clk_rate_hz);
	state->duty_cycle = DIV_ROUND_UP_ULL((u64)hlperiod_ticks * NSEC_PER_SEC,
					     ddata->clk_rate_hz);

	pwm_polarity = readl(ddata->base + SG2044_PWM_POLARITY);

	if (pwm_polarity & BIT(chan))
		state->polarity = PWM_POLARITY_NORMAL;
	else
		state->polarity = PWM_POLARITY_INVERSED;

	return 0;
}

static void pwm_cv1800b_set_pwmupdate(struct sg2042_pwm_ddata *ddata, struct pwm_device *pwm,
				      bool enable)
{
	u32 pwm_update = readl(ddata->base + SG2044_PWM_PWMUPDATE);

	/*
	 * Write 1 and then 0 to PWMUPDATE[n] to latch new HLPERIOD/PERIOD
	 * values while the channel is running.
	 */
	if (enable)
		pwm_update |= BIT(pwm->hwpwm);
	else
		pwm_update &= ~BIT(pwm->hwpwm);

	writel(pwm_update, ddata->base + SG2044_PWM_PWMUPDATE);
}

static void pwm_cv1800b_set_polarity(struct sg2042_pwm_ddata *ddata, struct pwm_device *pwm,
				     bool high)
{
	u32 pwm_polarity = readl(ddata->base + SG2044_PWM_POLARITY);

	/*
	 * POLARITY on CV1800B is the opposite of SG2044: 0 selects a low
	 * default level (HLPERIOD counts low cycles), 1 selects a high
	 * default level (HLPERIOD counts high cycles).
	 */
	if (high)
		pwm_polarity |= BIT(pwm->hwpwm);
	else
		pwm_polarity &= ~BIT(pwm->hwpwm);

	writel(pwm_polarity, ddata->base + SG2044_PWM_POLARITY);
}

static int pwm_cv1800b_set_dutycycle(struct pwm_chip *chip, struct pwm_device *pwm,
				     const struct pwm_state *state)
{
	struct sg2042_pwm_ddata *ddata = pwmchip_get_drvdata(chip);
	u32 hlperiod_ticks;
	u32 period_ticks;
	u64 ticks;

	/*
	 * Duration of High level (duty_cycle) = HLPERIOD x Period_of_input_clk
	 * Duration of One Cycle (period) = PERIOD x Period_of_input_clk
	 *
	 * Different from sg2042, cv1800b has max of 30 bits and PERIOD must be
	 * strictly greater than HLPERIOD.
	 */
	ticks = mul_u64_u64_div_u64(ddata->clk_rate_hz, state->period, NSEC_PER_SEC);
	period_ticks = min_t(u64, ticks, CV1800B_PWM_MAX_PERIOD);
	if (period_ticks < CV1800B_PWM_MIN_PERIOD)
		return -EINVAL;

	ticks = mul_u64_u64_div_u64(ddata->clk_rate_hz, state->duty_cycle, NSEC_PER_SEC);
	hlperiod_ticks = min_t(u64, ticks, period_ticks - 1);

	dev_dbg(pwmchip_parent(chip), "chan[%u]: ENABLE=%u, PERIOD=%u, HLPERIOD=%u, POLARITY=%u\n",
		pwm->hwpwm, state->enabled, period_ticks, hlperiod_ticks, state->polarity);

	pwm_sg2042_config(ddata, pwm->hwpwm, period_ticks, hlperiod_ticks);

	return 0;
}

static int pwm_cv1800b_apply(struct pwm_chip *chip, struct pwm_device *pwm,
			     const struct pwm_state *state)
{
	struct sg2042_pwm_ddata *ddata = pwmchip_get_drvdata(chip);
	bool high = state->polarity == PWM_POLARITY_NORMAL;
	bool running = pwm_sg2044_get_outputen(ddata, pwm);
	int ret;

	if (!state->enabled) {
		/*
		 * Stop the channel and drive the inactive level: low for
		 * normal polarity, high for inverted polarity.
		 */
		pwm_sg2044_set_outputen(ddata, pwm, false);
		pwm_cv1800b_set_polarity(ddata, pwm, !high);
		pwm_cv1800b_set_pwmupdate(ddata, pwm, false);
		pwm_sg2044_set_outputdir(ddata, pwm, true);

		return 0;
	}

	ret = pwm_cv1800b_set_dutycycle(chip, pwm, state);
	if (ret)
		return ret;

	pwm_cv1800b_set_polarity(ddata, pwm, high);

	if (!running) {
		pwm_cv1800b_set_pwmupdate(ddata, pwm, false);
		pwm_sg2044_set_outputdir(ddata, pwm, true);
		pwm_sg2044_set_outputen(ddata, pwm, false);
		pwm_sg2044_set_outputen(ddata, pwm, true);

		return 0;
	}

	pwm_cv1800b_set_pwmupdate(ddata, pwm, true);
	pwm_cv1800b_set_pwmupdate(ddata, pwm, false);

	return 0;
}

static const struct sg2042_chip_data cv1800b_chip_data = {
	.ops = {
		.apply = pwm_cv1800b_apply,
		.get_state = pwm_cv1800b_get_state,
	},
};

static const struct sg2042_chip_data sg2042_chip_data = {
	.ops = {
		.apply = pwm_sg2042_apply,
		.get_state = pwm_sg2042_get_state,
	},
};

static const struct sg2042_chip_data sg2044_chip_data = {
	.ops = {
		.apply = pwm_sg2044_apply,
		.get_state = pwm_sg2042_get_state,
	},
};

static const struct of_device_id sg2042_pwm_ids[] = {
	{
		.compatible = "sophgo,cv1800b-pwm",
		.data = &cv1800b_chip_data,
	}, {
		.compatible = "sophgo,sg2042-pwm",
		.data = &sg2042_chip_data,
	}, {
		.compatible = "sophgo,sg2044-pwm",
		.data = &sg2044_chip_data,
	},
	{ }
};
MODULE_DEVICE_TABLE(of, sg2042_pwm_ids);

static int pwm_sg2042_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const struct sg2042_chip_data *chip_data;
	struct sg2042_pwm_ddata *ddata;
	struct reset_control *rst;
	struct pwm_chip *chip;
	struct clk *clk;
	int ret;

	chip_data = device_get_match_data(dev);
	if (!chip_data)
		return -ENODEV;

	chip = devm_pwmchip_alloc(dev, SG2042_PWM_CHANNELNUM, sizeof(*ddata));
	if (IS_ERR(chip))
		return PTR_ERR(chip);
	ddata = pwmchip_get_drvdata(chip);

	ddata->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(ddata->base))
		return PTR_ERR(ddata->base);

	clk = devm_clk_get_enabled(dev, "apb");
	if (IS_ERR(clk))
		return dev_err_probe(dev, PTR_ERR(clk), "Failed to get base clk\n");

	ret = devm_clk_rate_exclusive_get(dev, clk);
	if (ret)
		return dev_err_probe(dev, ret, "Failed to get exclusive rate\n");

	ddata->clk_rate_hz = clk_get_rate(clk);
	/* period = PERIOD * NSEC_PER_SEC / clk_rate_hz */
	if (!ddata->clk_rate_hz || ddata->clk_rate_hz > NSEC_PER_SEC)
		return dev_err_probe(dev, -EINVAL,
				     "Invalid clock rate: %lu\n", ddata->clk_rate_hz);

	rst = devm_reset_control_get_optional_shared_deasserted(dev, NULL);
	if (IS_ERR(rst))
		return dev_err_probe(dev, PTR_ERR(rst), "Failed to get reset\n");

	chip->ops = &chip_data->ops;
	chip->atomic = true;

	ret = devm_pwmchip_add(dev, chip);
	if (ret < 0)
		return dev_err_probe(dev, ret, "Failed to register PWM chip\n");

	return 0;
}

static struct platform_driver pwm_sg2042_driver = {
	.driver	= {
		.name = "sg2042-pwm",
		.of_match_table = sg2042_pwm_ids,
	},
	.probe = pwm_sg2042_probe,
};
module_platform_driver(pwm_sg2042_driver);

MODULE_AUTHOR("Chen Wang");
MODULE_AUTHOR("Longbin Li <looong.bin@gmail.com>");
MODULE_AUTHOR("Hiago De Franco <hfranco@baylibre.com>");
MODULE_DESCRIPTION("Sophgo SG2042/SG2044/CV1800B PWM driver");
MODULE_LICENSE("GPL");
