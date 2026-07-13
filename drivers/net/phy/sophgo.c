// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for the integrated 10/100 ethernet PHY of the Sophgo
 * CV18xx/SG200x SoCs.
 *
 * The PHY has no vendor registers beyond the standard C22 set, but its
 * extended configuration lives in paged registers selected through
 * MII register 0x1f (page number in bits 15:8).
 *
 * The PHY ID registers read as zero out of reset; the PHY is matched
 * through the "ethernet-phy-id0043.5649" devicetree compatible.
 *
 * Copyright (C) 2026 Hiago De Franco <hfranco@baylibre.com>
 */

#include <linux/bitfield.h>
#include <linux/module.h>
#include <linux/phy.h>

#define PHY_ID_CV1800B_EPHY		0x00435649

/* Page number lives in bits 15:8 of the C22 page select register */
#define CV1800B_PAGE_SEL		0x1f

/* Page 1 */
#define CV1800B_LED_CTL			0x1a
#define CV1800B_LED_SRC_MASK		GENMASK(11, 8)

/* Page 5 */
#define CV1800B_PD_EN_CTL		0x10
#define CV1800B_EN_ETH_TXRT		BIT(0)
#define CV1800B_EN_ETH_CLK100M		BIT(1)
#define CV1800B_EN_ETH_CLK125M		BIT(2)
#define CV1800B_EN_ETH_PLL_LCKDET	BIT(3)
#define CV1800B_EN_ETH_RXADC		BIT(4)
#define CV1800B_EN_ETH_RXPGA		BIT(5)
#define CV1800B_EN_ETH_RXRT		BIT(6)
#define CV1800B_PD_ETH_PLL		BIT(8)
#define CV1800B_PD_ETH_TXDAC		BIT(9)
#define CV1800B_PD_ETH_TXDRV_NMOS	BIT(12)
#define CV1800B_PD_ETH_TXLDO		BIT(13)

#define CV1800B_ANALOG_EN	(CV1800B_EN_ETH_TXRT | \
				 CV1800B_EN_ETH_CLK100M | \
				 CV1800B_EN_ETH_CLK125M | \
				 CV1800B_EN_ETH_PLL_LCKDET | \
				 CV1800B_EN_ETH_RXADC | \
				 CV1800B_EN_ETH_RXPGA | \
				 CV1800B_EN_ETH_RXRT)
#define CV1800B_ANALOG_PD	(CV1800B_PD_ETH_PLL | \
				 CV1800B_PD_ETH_TXDAC | \
				 CV1800B_PD_ETH_TXDRV_NMOS | \
				 CV1800B_PD_ETH_TXLDO)

static int cv1800b_ephy_read_page(struct phy_device *phydev)
{
	int ret;

	ret = __phy_read(phydev, CV1800B_PAGE_SEL);
	if (ret < 0)
		return ret;

	return FIELD_GET(GENMASK(15, 8), ret);
}

static int cv1800b_ephy_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, CV1800B_PAGE_SEL,
			   FIELD_PREP(GENMASK(15, 8), page));
}

static int cv1800b_ephy_config_init(struct phy_device *phydev)
{
	int ret;

	/* Power up the analog blocks */
	ret = phy_modify_paged(phydev, 5, CV1800B_PD_EN_CTL,
			       CV1800B_ANALOG_EN | CV1800B_ANALOG_PD,
			       CV1800B_ANALOG_EN);
	if (ret < 0)
		return ret;

	/* Output link/speed/duplex indication on the LED pad */
	return phy_modify_paged(phydev, 1, CV1800B_LED_CTL,
				CV1800B_LED_SRC_MASK, 0);
}

static struct phy_driver cv1800b_ephy_driver[] = {
	{
		PHY_ID_MATCH_EXACT(PHY_ID_CV1800B_EPHY),
		.name		= "Sophgo CV18xx internal EPHY",
		.config_init	= cv1800b_ephy_config_init,
		.read_page	= cv1800b_ephy_read_page,
		.write_page	= cv1800b_ephy_write_page,
		.suspend	= genphy_suspend,
		.resume		= genphy_resume,
	},
};
module_phy_driver(cv1800b_ephy_driver);

static const struct mdio_device_id __maybe_unused cv1800b_ephy_tbl[] = {
	{ PHY_ID_MATCH_EXACT(PHY_ID_CV1800B_EPHY) },
	{ }
};
MODULE_DEVICE_TABLE(mdio, cv1800b_ephy_tbl);

MODULE_DESCRIPTION("Sophgo CV18xx/SG200x internal ethernet PHY driver");
MODULE_AUTHOR("Hiago De Franco <hfranco@baylibre.com>");
MODULE_LICENSE("GPL");
