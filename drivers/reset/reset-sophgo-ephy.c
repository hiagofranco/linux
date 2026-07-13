// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2026 Hiago De Franco <hfranco@baylibre.com>
 */

#include <linux/device.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/reset-controller.h>

#define EPHY_CTRL_REG	0x0
#define EPHY_SHUTDOWN	BIT(0)
#define EPHY_ANA_RST_N	BIT(1)
#define EPHY_DIG_RST_N	BIT(2)
#define EPHY_MAIN_RST_N	BIT(3)
#define EPHY_RST_MASK	GENMASK(3, 0)

struct sophgo_ephy_reset_priv {
	struct reset_controller_dev rcdev;
	struct regmap *regmap;
};

static inline struct sophgo_ephy_reset_priv*
to_sophgo_ephy_reset(struct reset_controller_dev *rcdev)
{
	return container_of_const(rcdev, struct sophgo_ephy_reset_priv, rcdev);
}

static int sophgo_ephy_reset_assert(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	struct sophgo_ephy_reset_priv *priv = to_sophgo_ephy_reset(rcdev);

	return regmap_update_bits(priv->regmap, EPHY_CTRL_REG, EPHY_RST_MASK,
				  EPHY_SHUTDOWN);
}

static int sophgo_ephy_reset_deassert(struct reset_controller_dev *rcdev,
				     unsigned long id)
{
	struct sophgo_ephy_reset_priv *priv = to_sophgo_ephy_reset(rcdev);
	int val = EPHY_ANA_RST_N | EPHY_DIG_RST_N | EPHY_MAIN_RST_N;

	return regmap_update_bits(priv->regmap, EPHY_CTRL_REG, EPHY_RST_MASK,
				  val);
}

static int sophgo_ephy_reset_status(struct reset_controller_dev *rcdev,
				   unsigned long id)
{
	struct sophgo_ephy_reset_priv *priv = to_sophgo_ephy_reset(rcdev);
	unsigned int val;
	int ret;

	ret = regmap_read(priv->regmap, EPHY_CTRL_REG, &val);
	if (ret < 0)
		return ret;
	return !!(val & EPHY_SHUTDOWN);
}

static const struct reset_control_ops sophgo_ephy_reset_ops = {
	.assert	= sophgo_ephy_reset_assert,
	.deassert = sophgo_ephy_reset_deassert,
	.status = sophgo_ephy_reset_status,
};

static int sophgo_ephy_reset_of_xlate(struct reset_controller_dev *rcdev,
				      const struct of_phandle_args *reset_spec)
{
	/* No special handling needed, we have only one reset line per device */
	return 0;
}

static int sophgo_ephy_reset_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct sophgo_ephy_reset_priv *priv;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->regmap = syscon_node_to_regmap(dev->of_node);
	if (IS_ERR(priv->regmap))
		return PTR_ERR(priv->regmap);

	priv->rcdev.ops = &sophgo_ephy_reset_ops;
	priv->rcdev.owner = THIS_MODULE;
	priv->rcdev.dev = dev;
	priv->rcdev.of_node = dev->of_node;
	priv->rcdev.of_reset_n_cells = 0;
	priv->rcdev.of_xlate = sophgo_ephy_reset_of_xlate;
	priv->rcdev.nr_resets = 1;

	return devm_reset_controller_register(dev, &priv->rcdev);
}

static const struct of_device_id sophgo_ephy_reset_match[] = {
	{ .compatible = "sophgo,cv1800b-ephy-ctrl" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, sophgo_ephy_reset_match);

static struct platform_driver sophgo_ephy_reset_driver = {
	.driver = {
		.name = "reset-sophgo-ephy",
		.of_match_table = sophgo_ephy_reset_match,
	},
	.probe = sophgo_ephy_reset_probe,
};
module_platform_driver(sophgo_ephy_reset_driver);

MODULE_AUTHOR("Hiago De Franco <hfranco@baylibre.com>");
MODULE_DESCRIPTION("Sophgo CV18xx/SG2002 EPHY reset controller");
MODULE_LICENSE("GPL");
