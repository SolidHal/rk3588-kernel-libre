// SPDX-License-Identifier: ISC
/* Copyright (C) 2020 MediaTek Inc. */

#include <linux/firmware.h>
#include "mt7915.h"
#include "eeprom.h"

static int mt7915_eeprom_load_precal(struct mt7915_dev *dev)
{
	struct mt76_dev *mdev = &dev->mt76;
	u8 *eeprom = mdev->eeprom.data;
	u32 val = eeprom[MT_EE_DO_PRE_CAL];
	u32 offs;

	if (!dev->flash_mode)
		return 0;

	if (val != (MT_EE_WIFI_CAL_DPD | MT_EE_WIFI_CAL_GROUP))
		return 0;

	val = MT_EE_CAL_GROUP_SIZE + MT_EE_CAL_DPD_SIZE;
	dev->cal = devm_kzalloc(mdev->dev, val, GFP_KERNEL);
	if (!dev->cal)
		return -ENOMEM;

	offs = is_mt7915(&dev->mt76) ? MT_EE_PRECAL : MT_EE_PRECAL_V2;

	return mt76_get_of_eeprom(mdev, dev->cal, offs, val);
}

static int mt7915_check_eeprom(struct mt7915_dev *dev)
{
	u8 *eeprom = dev->mt76.eeprom.data;
	u16 val = get_unaligned_le16(eeprom);

	switch (val) {
	case 0x7915:
	case 0x7916:
	case 0x7986:
		return 0;
	default:
		return -EINVAL;
	}
}

static char *mt7915_eeprom_name(struct mt7915_dev *dev)
{
	switch (mt76_chip(&dev->mt76)) {
	case 0x7915:
		return dev->dbdc_support ?
		       MT7915_EEPROM_DEFAULT_DBDC : MT7915_EEPROM_DEFAULT;
	case 0x7986:
		switch (mt7915_check_adie(dev, true)) {
		case MT7976_ONE_ADIE_DBDC:
			return MT7986_EEPROM_MT7976_DEFAULT_DBDC;
		case MT7975_ONE_ADIE:
			return MT7986_EEPROM_MT7975_DEFAULT;
		case MT7976_ONE_ADIE:
			return MT7986_EEPROM_MT7976_DEFAULT;
		case MT7975_DUAL_ADIE:
			return MT7986_EEPROM_MT7975_DUAL_DEFAULT;
		case MT7976_DUAL_ADIE:
			return MT7986_EEPROM_MT7976_DUAL_DEFAULT;
		default:
			break;
		}
		return NULL;
	default:
		return MT7916_EEPROM_DEFAULT;
	}
}

static int
mt7915_eeprom_load_default(struct mt7915_dev *dev)
{
	u8 *eeprom = dev->mt76.eeprom.data;
	const struct firmware *fw = NULL;
	int ret;

	ret = reject_firmware(&fw, mt7915_eeprom_name(dev), dev->mt76.dev);
	if (ret)
		return ret;

	if (!fw || !fw->data) {
		dev_err(dev->mt76.dev, "Invalid default bin\n");
		ret = -EINVAL;
		goto out;
	}

	memcpy(eeprom, fw->data, mt7915_eeprom_size(dev));
	dev->flash_mode = true;

out:
	release_firmware(fw);

	return ret;
}

static int mt7915_eeprom_load(struct mt7915_dev *dev)
{
	int ret;
	u16 eeprom_size = mt7915_eeprom_size(dev);

	ret = mt76_eeprom_init(&dev->mt76, eeprom_size);
	if (ret < 0)
		return ret;

	if (ret) {
		dev->flash_mode = true;
	} else {
		u8 free_block_num;
		u32 block_num, i;

		mt7915_mcu_get_eeprom_free_block(dev, &free_block_num);
		/* efuse info not enough */
		if (free_block_num >= 29)
			return -EINVAL;

		/* read eeprom data from efuse */
		block_num = DIV_ROUND_UP(eeprom_size,
					 MT7915_EEPROM_BLOCK_SIZE);
		for (i = 0; i < block_num; i++)
			mt7915_mcu_get_eeprom(dev,
					      i * MT7915_EEPROM_BLOCK_SIZE);
	}

	return mt7915_check_eeprom(dev);
}

static void mt7915_eeprom_parse_band_config(struct mt7915_phy *phy)
{
	struct mt7915_dev *dev = phy->dev;
	u8 *eeprom = dev->mt76.eeprom.data;
	u32 val;

	val = eeprom[MT_EE_WIFI_CONF + phy->band_idx];
	val = FIELD_GET(MT_EE_WIFI_CONF0_BAND_SEL, val);

	if (!is_mt7915(&dev->mt76)) {
		switch (val) {
		case MT_EE_V2_BAND_SEL_5GHZ:
			phy->mt76->cap.has_5ghz = true;
			return;
		case MT_EE_V2_BAND_SEL_6GHZ:
			phy->mt76->cap.has_6ghz = true;
			return;
		case MT_EE_V2_BAND_SEL_5GHZ_6GHZ:
			phy->mt76->cap.has_5ghz = true;
			phy->mt76->cap.has_6ghz = true;
			return;
		default:
			phy->mt76->cap.has_2ghz = true;
			return;
		}
	} else if (val == MT_EE_BAND_SEL_DEFAULT && dev->dbdc_support) {
		val = phy->band_idx ? MT_EE_BAND_SEL_5GHZ : MT_EE_BAND_SEL_2GHZ;
	}

	switch (val) {
	case MT_EE_BAND_SEL_5GHZ:
		phy->mt76->cap.has_5ghz = true;
		break;
	case MT_EE_BAND_SEL_2GHZ:
		phy->mt76->cap.has_2ghz = true;
		break;
	default:
		phy->mt76->cap.has_2ghz = true;
		phy->mt76->cap.has_5ghz = true;
		break;
	}
}

void mt7915_eeprom_parse_hw_cap(struct mt7915_dev *dev,
				struct mt7915_phy *phy)
{
	u8 nss, nss_band, nss_band_max, *eeprom = dev->mt76.eeprom.data;
	struct mt76_phy *mphy = phy->mt76;
	bool ext_phy = phy != &dev->phy;

	mt7915_eeprom_parse_band_config(phy);

	/* read tx/rx mask from eeprom */
	if (is_mt7915(&dev->mt76)) {
		nss = FIELD_GET(MT_EE_WIFI_CONF0_TX_PATH,
				eeprom[MT_EE_WIFI_CONF]);
	} else {
		nss = FIELD_GET(MT_EE_WIFI_CONF0_TX_PATH,
				eeprom[MT_EE_WIFI_CONF + phy->band_idx]);
	}

	if (!nss || nss > 4)
		nss = 4;

	/* read tx/rx stream */
	nss_band = nss;

	if (dev->dbdc_support) {
		if (is_mt7915(&dev->mt76)) {
			nss_band = FIELD_GET(MT_EE_WIFI_CONF3_TX_PATH_B0,
					     eeprom[MT_EE_WIFI_CONF + 3]);
			if (phy->band_idx)
				nss_band = FIELD_GET(MT_EE_WIFI_CONF3_TX_PATH_B1,
						     eeprom[MT_EE_WIFI_CONF + 3]);
		} else {
			nss_band = FIELD_GET(MT_EE_WIFI_CONF_STREAM_NUM,
					     eeprom[MT_EE_WIFI_CONF + 2 + phy->band_idx]);
		}

		nss_band_max = is_mt7986(&dev->mt76) ?
			       MT_EE_NSS_MAX_DBDC_MA7986 : MT_EE_NSS_MAX_DBDC_MA7915;
	} else {
		nss_band_max = is_mt7986(&dev->mt76) ?
			       MT_EE_NSS_MAX_MA7986 : MT_EE_NSS_MAX_MA7915;
	}

	if (!nss_band || nss_band > nss_band_max)
		nss_band = nss_band_max;

	if (nss_band > nss) {
		dev_warn(dev->mt76.dev,
			 "nss mismatch, nss(%d) nss_band(%d) band(%d) ext_phy(%d)\n",
			 nss, nss_band, phy->band_idx, ext_phy);
		nss = nss_band;
	}

	mphy->chainmask = BIT(nss) - 1;
	if (ext_phy)
		mphy->chainmask <<= dev->chainshift;
	mphy->antenna_mask = BIT(nss_band) - 1;
	dev->chainmask |= mphy->chainmask;
	dev->chainshift = hweight8(dev->mphy.chainmask);
}

int mt7915_eeprom_init(struct mt7915_dev *dev)
{
	int ret;

	ret = mt7915_eeprom_load(dev);
	if (ret < 0) {
		if (ret != -EINVAL)
			return ret;

		dev_warn(dev->mt76.dev, "eeprom load fail, use default bin\n");
		ret = mt7915_eeprom_load_default(dev);
		if (ret)
			return ret;
	}

	ret = mt7915_eeprom_load_precal(dev);
	if (ret)
		return ret;

	mt7915_eeprom_parse_hw_cap(dev, &dev->phy);
	memcpy(dev->mphy.macaddr, dev->mt76.eeprom.data + MT_EE_MAC_ADDR,
	       ETH_ALEN);

	mt76_eeprom_override(&dev->mphy);

	return 0;
}

int mt7915_eeprom_get_target_power(struct mt7915_dev *dev,
				   struct ieee80211_channel *chan,
				   u8 chain_idx)
{
	u8 *eeprom = dev->mt76.eeprom.data;
	int index, target_power;
	bool tssi_on, is_7976;

	if (chain_idx > 3)
		return -EINVAL;

	tssi_on = mt7915_tssi_enabled(dev, chan->band);
	is_7976 = mt7915_check_adie(dev, false) || is_mt7916(&dev->mt76);

	if (chan->band == NL80211_BAND_2GHZ) {
		if (is_7976) {
			index = MT_EE_TX0_POWER_2G_V2 + chain_idx;
			target_power = eeprom[index];
		} else {
			index = MT_EE_TX0_POWER_2G + chain_idx * 3;
			target_power = eeprom[index];

			if (!tssi_on)
				target_power += eeprom[index + 1];
		}
	} else if (chan->band == NL80211_BAND_5GHZ) {
		int group = mt7915_get_channel_group_5g(chan->hw_value, is_7976);

		if (is_7976) {
			index = MT_EE_TX0_POWER_5G_V2 + chain_idx * 5;
			target_power = eeprom[index + group];
		} else {
			index = MT_EE_TX0_POWER_5G + chain_idx * 12;
			target_power = eeprom[index + group];

			if (!tssi_on)
				target_power += eeprom[index + 8];
		}
	} else {
		int group = mt7915_get_channel_group_6g(chan->hw_value);

		index = MT_EE_TX0_POWER_6G_V2 + chain_idx * 8;
		target_power = is_7976 ? eeprom[index + group] : 0;
	}

	return target_power;
}

s8 mt7915_eeprom_get_power_delta(struct mt7915_dev *dev, int band)
{
	u8 *eeprom = dev->mt76.eeprom.data;
	u32 val, offs;
	s8 delta;
	bool is_7976 = mt7915_check_adie(dev, false) || is_mt7916(&dev->mt76);

	if (band == NL80211_BAND_2GHZ)
		offs = is_7976 ? MT_EE_RATE_DELTA_2G_V2 : MT_EE_RATE_DELTA_2G;
	else if (band == NL80211_BAND_5GHZ)
		offs = is_7976 ? MT_EE_RATE_DELTA_5G_V2 : MT_EE_RATE_DELTA_5G;
	else
		offs = is_7976 ? MT_EE_RATE_DELTA_6G_V2 : 0;

	val = eeprom[offs];

	if (!offs || !(val & MT_EE_RATE_DELTA_EN))
		return 0;

	delta = FIELD_GET(MT_EE_RATE_DELTA_MASK, val);

	return val & MT_EE_RATE_DELTA_SIGN ? delta : -delta;
}

const u8 mt7915_sku_group_len[] = {
	[SKU_CCK] = 4,
	[SKU_OFDM] = 8,
	[SKU_HT_BW20] = 8,
	[SKU_HT_BW40] = 9,
	[SKU_VHT_BW20] = 12,
	[SKU_VHT_BW40] = 12,
	[SKU_VHT_BW80] = 12,
	[SKU_VHT_BW160] = 12,
	[SKU_HE_RU26] = 12,
	[SKU_HE_RU52] = 12,
	[SKU_HE_RU106] = 12,
	[SKU_HE_RU242] = 12,
	[SKU_HE_RU484] = 12,
	[SKU_HE_RU996] = 12,
	[SKU_HE_RU2x996] = 12
};
