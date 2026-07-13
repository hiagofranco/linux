// SPDX-License-Identifier: GPL-2.0-only
/*
 * Reset driver for the integrated ethernet PHY of the Sophgo
 * CV18xx/SG200x SoCs.
 *
 * The PHY wrapper exposes a single control register combining an
 * active-high shutdown bit with three active-low reset bits, all of
 * which have to be toggled together to power the PHY up or down.
 * The same register also carries the internal/external MDIO mux bit
 * (handled by an mmio-mux instance on the same syscon regmap).
 *
 * Copyright (C) 2026 Hiago De Franco <hfranco@baylibre.com>
 */

#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define EPHY_CTL	0x0
#define EPHY_SHUTDOWN	BIT(0)
#define EPHY_ANA_RST_N	BIT(1)
#define EPHY_DIG_RST_N	BIT(2)
#define EPHY_MAIN_RST_N	BIT(3)
#define EPHY_CTL_MASK	GENMASK(3, 0)
#define EPHY_RST_N	(EPHY_ANA_RST_N | EPHY_DIG_RST_N | EPHY_MAIN_RST_N)

struct cv1800b_ephy_reset {
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
};

static struct cv1800b_ephy_reset *
to_cv1800b_ephy_reset(struct reset_controller_dev *rcdev)
{
	return container_of(rcdev, struct cv1800b_ephy_reset, rcdev);
}

static int cv1800b_ephy_reset_assert(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	struct cv1800b_ephy_reset *priv = to_cv1800b_ephy_reset(rcdev);

	return regmap_update_bits(priv->regmap, EPHY_CTL, EPHY_CTL_MASK,
				  EPHY_SHUTDOWN);
}

static int cv1800b_ephy_reset_deassert(struct reset_controller_dev *rcdev,
				       unsigned long id)
{
	struct cv1800b_ephy_reset *priv = to_cv1800b_ephy_reset(rcdev);

	return regmap_update_bits(priv->regmap, EPHY_CTL, EPHY_CTL_MASK,
				  EPHY_RST_N);
}

static int cv1800b_ephy_reset_status(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	struct cv1800b_ephy_reset *priv = to_cv1800b_ephy_reset(rcdev);
	u32 val;
	int ret;

	ret = regmap_read(priv->regmap, EPHY_CTL, &val);
	if (ret)
		return ret;

	return (val & EPHY_CTL_MASK) != EPHY_RST_N;
}

static const struct reset_control_ops cv1800b_ephy_reset_ops = {
	.assert		= cv1800b_ephy_reset_assert,
	.deassert	= cv1800b_ephy_reset_deassert,
	.status		= cv1800b_ephy_reset_status,
};

static int cv1800b_ephy_reset_xlate(struct reset_controller_dev *rcdev,
				    const struct of_phandle_args *reset_spec)
{
	return 0;
}

static int cv1800b_ephy_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct cv1800b_ephy_reset *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->rcdev.ops = &cv1800b_ephy_reset_ops;
	priv->rcdev.owner = THIS_MODULE;
	priv->rcdev.of_node = dev->of_node;
	priv->rcdev.of_reset_n_cells = 0;
	priv->rcdev.of_xlate = cv1800b_ephy_reset_xlate;
	priv->rcdev.nr_resets = 1;

	return devm_reset_controller_register(dev, &priv->rcdev);
}

static const struct of_device_id cv1800b_ephy_reset_of_match[] = {
	{ .compatible = "sophgo,cv1800b-ephy-ctrl" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cv1800b_ephy_reset_of_match);

static struct platform_driver cv1800b_ephy_reset_driver = {
	.probe	= cv1800b_ephy_reset_probe,
	.driver	= {
		.name		= "cv1800b-ephy-reset",
		.of_match_table	= cv1800b_ephy_reset_of_match,
	},
};
module_platform_driver(cv1800b_ephy_reset_driver);

MODULE_DESCRIPTION("Sophgo CV18xx/SG200x ethernet PHY reset driver");
MODULE_AUTHOR("Hiago De Franco <hfranco@baylibre.com>");
MODULE_LICENSE("GPL");
