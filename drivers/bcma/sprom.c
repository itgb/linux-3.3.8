/*
 * Broadcom specific AMBA
 * SPROM reading
 *
 * Licensed under the GNU/GPL. See COPYING for details.
 */

#include "bcma_private.h"

#include <linux/bcma/bcma.h>
#include <linux/bcma/bcma_regs.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>

#define SPOFF(offset)	((offset) / sizeof(u16))

/**************************************************
 * R/W ops.
 **************************************************/

static void bcma_sprom_read(struct bcma_bus *bus, u16 offset, u16 *sprom)
{
	int i;
	for (i = 0; i < SSB_SPROMSIZE_WORDS_R4; i++)
		sprom[i] = bcma_read16(bus->drv_cc.core,
				       offset + (i * 2));
}

/**************************************************
 * Validation.
 **************************************************/

static inline u8 bcma_crc8(u8 crc, u8 data)
{
	/* Polynomial:   x^8 + x^7 + x^6 + x^4 + x^2 + 1   */
	static const u8 t[] = {
		0x00, 0xF7, 0xB9, 0x4E, 0x25, 0xD2, 0x9C, 0x6B,
		0x4A, 0xBD, 0xF3, 0x04, 0x6F, 0x98, 0xD6, 0x21,
		0x94, 0x63, 0x2D, 0xDA, 0xB1, 0x46, 0x08, 0xFF,
		0xDE, 0x29, 0x67, 0x90, 0xFB, 0x0C, 0x42, 0xB5,
		0x7F, 0x88, 0xC6, 0x31, 0x5A, 0xAD, 0xE3, 0x14,
		0x35, 0xC2, 0x8C, 0x7B, 0x10, 0xE7, 0xA9, 0x5E,
		0xEB, 0x1C, 0x52, 0xA5, 0xCE, 0x39, 0x77, 0x80,
		0xA1, 0x56, 0x18, 0xEF, 0x84, 0x73, 0x3D, 0xCA,
		0xFE, 0x09, 0x47, 0xB0, 0xDB, 0x2C, 0x62, 0x95,
		0xB4, 0x43, 0x0D, 0xFA, 0x91, 0x66, 0x28, 0xDF,
		0x6A, 0x9D, 0xD3, 0x24, 0x4F, 0xB8, 0xF6, 0x01,
		0x20, 0xD7, 0x99, 0x6E, 0x05, 0xF2, 0xBC, 0x4B,
		0x81, 0x76, 0x38, 0xCF, 0xA4, 0x53, 0x1D, 0xEA,
		0xCB, 0x3C, 0x72, 0x85, 0xEE, 0x19, 0x57, 0xA0,
		0x15, 0xE2, 0xAC, 0x5B, 0x30, 0xC7, 0x89, 0x7E,
		0x5F, 0xA8, 0xE6, 0x11, 0x7A, 0x8D, 0xC3, 0x34,
		0xAB, 0x5C, 0x12, 0xE5, 0x8E, 0x79, 0x37, 0xC0,
		0xE1, 0x16, 0x58, 0xAF, 0xC4, 0x33, 0x7D, 0x8A,
		0x3F, 0xC8, 0x86, 0x71, 0x1A, 0xED, 0xA3, 0x54,
		0x75, 0x82, 0xCC, 0x3B, 0x50, 0xA7, 0xE9, 0x1E,
		0xD4, 0x23, 0x6D, 0x9A, 0xF1, 0x06, 0x48, 0xBF,
		0x9E, 0x69, 0x27, 0xD0, 0xBB, 0x4C, 0x02, 0xF5,
		0x40, 0xB7, 0xF9, 0x0E, 0x65, 0x92, 0xDC, 0x2B,
		0x0A, 0xFD, 0xB3, 0x44, 0x2F, 0xD8, 0x96, 0x61,
		0x55, 0xA2, 0xEC, 0x1B, 0x70, 0x87, 0xC9, 0x3E,
		0x1F, 0xE8, 0xA6, 0x51, 0x3A, 0xCD, 0x83, 0x74,
		0xC1, 0x36, 0x78, 0x8F, 0xE4, 0x13, 0x5D, 0xAA,
		0x8B, 0x7C, 0x32, 0xC5, 0xAE, 0x59, 0x17, 0xE0,
		0x2A, 0xDD, 0x93, 0x64, 0x0F, 0xF8, 0xB6, 0x41,
		0x60, 0x97, 0xD9, 0x2E, 0x45, 0xB2, 0xFC, 0x0B,
		0xBE, 0x49, 0x07, 0xF0, 0x9B, 0x6C, 0x22, 0xD5,
		0xF4, 0x03, 0x4D, 0xBA, 0xD1, 0x26, 0x68, 0x9F,
	};
	return t[crc ^ data];
}

static u8 bcma_sprom_crc(const u16 *sprom)
{
	int word;
	u8 crc = 0xFF;

	for (word = 0; word < SSB_SPROMSIZE_WORDS_R4 - 1; word++) {
		crc = bcma_crc8(crc, sprom[word] & 0x00FF);
		crc = bcma_crc8(crc, (sprom[word] & 0xFF00) >> 8);
	}
	crc = bcma_crc8(crc, sprom[SSB_SPROMSIZE_WORDS_R4 - 1] & 0x00FF);
	crc ^= 0xFF;

	return crc;
}

static int bcma_sprom_check_crc(const u16 *sprom)
{
	u8 crc;
	u8 expected_crc;
	u16 tmp;

	crc = bcma_sprom_crc(sprom);
	tmp = sprom[SSB_SPROMSIZE_WORDS_R4 - 1] & SSB_SPROM_REVISION_CRC;
	expected_crc = tmp >> SSB_SPROM_REVISION_CRC_SHIFT;
	if (crc != expected_crc)
		return -EPROTO;

	return 0;
}

static int bcma_sprom_valid(const u16 *sprom)
{
	u16 revision;
	int err;

	err = bcma_sprom_check_crc(sprom);
	if (err)
		return err;

	revision = sprom[SSB_SPROMSIZE_WORDS_R4 - 1] & SSB_SPROM_REVISION_REV;
	if (revision != 8 && revision != 9) {
		pr_err("Unsupported SPROM revision: %d\n", revision);
		return -ENOENT;
	}

	return 0;
}

/**************************************************
 * SPROM extraction.
 **************************************************/

static void bcma_sprom_extract_r8(struct bcma_bus *bus, const u16 *sprom)
{
	u16 v;
	int i;

	bus->sprom.revision = sprom[SSB_SPROMSIZE_WORDS_R4 - 1] &
		SSB_SPROM_REVISION_REV;

	for (i = 0; i < 3; i++) {
		v = sprom[SPOFF(SSB_SPROM8_IL0MAC) + i];
		*(((__be16 *)bus->sprom.il0mac) + i) = cpu_to_be16(v);
	}

	bus->sprom.board_rev = sprom[SPOFF(SSB_SPROM8_BOARDREV)];

	bus->sprom.txpid2g[0] = (sprom[SPOFF(SSB_SPROM4_TXPID2G01)] &
	     SSB_SPROM4_TXPID2G0) >> SSB_SPROM4_TXPID2G0_SHIFT;
	bus->sprom.txpid2g[1] = (sprom[SPOFF(SSB_SPROM4_TXPID2G01)] &
	     SSB_SPROM4_TXPID2G1) >> SSB_SPROM4_TXPID2G1_SHIFT;
	bus->sprom.txpid2g[2] = (sprom[SPOFF(SSB_SPROM4_TXPID2G23)] &
	     SSB_SPROM4_TXPID2G2) >> SSB_SPROM4_TXPID2G2_SHIFT;
	bus->sprom.txpid2g[3] = (sprom[SPOFF(SSB_SPROM4_TXPID2G23)] &
	     SSB_SPROM4_TXPID2G3) >> SSB_SPROM4_TXPID2G3_SHIFT;

	bus->sprom.txpid5gl[0] = (sprom[SPOFF(SSB_SPROM4_TXPID5GL01)] &
	     SSB_SPROM4_TXPID5GL0) >> SSB_SPROM4_TXPID5GL0_SHIFT;
	bus->sprom.txpid5gl[1] = (sprom[SPOFF(SSB_SPROM4_TXPID5GL01)] &
	     SSB_SPROM4_TXPID5GL1) >> SSB_SPROM4_TXPID5GL1_SHIFT;
	bus->sprom.txpid5gl[2] = (sprom[SPOFF(SSB_SPROM4_TXPID5GL23)] &
	     SSB_SPROM4_TXPID5GL2) >> SSB_SPROM4_TXPID5GL2_SHIFT;
	bus->sprom.txpid5gl[3] = (sprom[SPOFF(SSB_SPROM4_TXPID5GL23)] &
	     SSB_SPROM4_TXPID5GL3) >> SSB_SPROM4_TXPID5GL3_SHIFT;

	bus->sprom.txpid5g[0] = (sprom[SPOFF(SSB_SPROM4_TXPID5G01)] &
	     SSB_SPROM4_TXPID5G0) >> SSB_SPROM4_TXPID5G0_SHIFT;
	bus->sprom.txpid5g[1] = (sprom[SPOFF(SSB_SPROM4_TXPID5G01)] &
	     SSB_SPROM4_TXPID5G1) >> SSB_SPROM4_TXPID5G1_SHIFT;
	bus->sprom.txpid5g[2] = (sprom[SPOFF(SSB_SPROM4_TXPID5G23)] &
	     SSB_SPROM4_TXPID5G2) >> SSB_SPROM4_TXPID5G2_SHIFT;
	bus->sprom.txpid5g[3] = (sprom[SPOFF(SSB_SPROM4_TXPID5G23)] &
	     SSB_SPROM4_TXPID5G3) >> SSB_SPROM4_TXPID5G3_SHIFT;

	bus->sprom.txpid5gh[0] = (sprom[SPOFF(SSB_SPROM4_TXPID5GH01)] &
	     SSB_SPROM4_TXPID5GH0) >> SSB_SPROM4_TXPID5GH0_SHIFT;
	bus->sprom.txpid5gh[1] = (sprom[SPOFF(SSB_SPROM4_TXPID5GH01)] &
	     SSB_SPROM4_TXPID5GH1) >> SSB_SPROM4_TXPID5GH1_SHIFT;
	bus->sprom.txpid5gh[2] = (sprom[SPOFF(SSB_SPROM4_TXPID5GH23)] &
	     SSB_SPROM4_TXPID5GH2) >> SSB_SPROM4_TXPID5GH2_SHIFT;
	bus->sprom.txpid5gh[3] = (sprom[SPOFF(SSB_SPROM4_TXPID5GH23)] &
	     SSB_SPROM4_TXPID5GH3) >> SSB_SPROM4_TXPID5GH3_SHIFT;

	bus->sprom.boardflags_lo = sprom[SPOFF(SSB_SPROM8_BFLLO)];
	bus->sprom.boardflags_hi = sprom[SPOFF(SSB_SPROM8_BFLHI)];
	bus->sprom.boardflags2_lo = sprom[SPOFF(SSB_SPROM8_BFL2LO)];
	bus->sprom.boardflags2_hi = sprom[SPOFF(SSB_SPROM8_BFL2HI)];

	bus->sprom.country_code = sprom[SPOFF(SSB_SPROM8_CCODE)];

	bus->sprom.fem.ghz2.tssipos = (sprom[SPOFF(SSB_SPROM8_FEM2G)] &
		SSB_SROM8_FEM_TSSIPOS) >> SSB_SROM8_FEM_TSSIPOS_SHIFT;
	bus->sprom.fem.ghz2.extpa_gain = (sprom[SPOFF(SSB_SPROM8_FEM2G)] &
		SSB_SROM8_FEM_EXTPA_GAIN) >> SSB_SROM8_FEM_EXTPA_GAIN_SHIFT;
	bus->sprom.fem.ghz2.pdet_range = (sprom[SPOFF(SSB_SPROM8_FEM2G)] &
		SSB_SROM8_FEM_PDET_RANGE) >> SSB_SROM8_FEM_PDET_RANGE_SHIFT;
	bus->sprom.fem.ghz2.tr_iso = (sprom[SPOFF(SSB_SPROM8_FEM2G)] &
		SSB_SROM8_FEM_TR_ISO) >> SSB_SROM8_FEM_TR_ISO_SHIFT;
	bus->sprom.fem.ghz2.antswlut = (sprom[SPOFF(SSB_SPROM8_FEM2G)] &
		SSB_SROM8_FEM_ANTSWLUT) >> SSB_SROM8_FEM_ANTSWLUT_SHIFT;

	bus->sprom.fem.ghz5.tssipos = (sprom[SPOFF(SSB_SPROM8_FEM5G)] &
		SSB_SROM8_FEM_TSSIPOS) >> SSB_SROM8_FEM_TSSIPOS_SHIFT;
	bus->sprom.fem.ghz5.extpa_gain = (sprom[SPOFF(SSB_SPROM8_FEM5G)] &
		SSB_SROM8_FEM_EXTPA_GAIN) >> SSB_SROM8_FEM_EXTPA_GAIN_SHIFT;
	bus->sprom.fem.ghz5.pdet_range = (sprom[SPOFF(SSB_SPROM8_FEM5G)] &
		SSB_SROM8_FEM_PDET_RANGE) >> SSB_SROM8_FEM_PDET_RANGE_SHIFT;
	bus->sprom.fem.ghz5.tr_iso = (sprom[SPOFF(SSB_SPROM8_FEM5G)] &
		SSB_SROM8_FEM_TR_ISO) >> SSB_SROM8_FEM_TR_ISO_SHIFT;
	bus->sprom.fem.ghz5.antswlut = (sprom[SPOFF(SSB_SPROM8_FEM5G)] &
		SSB_SROM8_FEM_ANTSWLUT) >> SSB_SROM8_FEM_ANTSWLUT_SHIFT;
}

int bcma_sprom_get(struct bcma_bus *bus)
{
	u16 offset;
	u16 *sprom;
	int err = 0;

	if (!bus->drv_cc.core)
		return -EOPNOTSUPP;

	if (!(bus->drv_cc.capabilities & BCMA_CC_CAP_SPROM))
		return -ENOENT;

	sprom = kcalloc(SSB_SPROMSIZE_WORDS_R4, sizeof(u16),
			GFP_KERNEL);
	if (!sprom)
		return -ENOMEM;

	if (bus->chipinfo.id == 0x4331)
		bcma_chipco_bcm4331_ext_pa_lines_ctl(&bus->drv_cc, false);

	/* Most cards have SPROM moved by additional offset 0x30 (48 dwords).
	 * According to brcm80211 this applies to cards with PCIe rev >= 6
	 * TODO: understand this condition and use it */
	offset = (bus->chipinfo.id == 0x4331) ? BCMA_CC_SPROM :
		BCMA_CC_SPROM_PCIE6;
	bcma_sprom_read(bus, offset, sprom);

	if (bus->chipinfo.id == 0x4331)
		bcma_chipco_bcm4331_ext_pa_lines_ctl(&bus->drv_cc, true);

	err = bcma_sprom_valid(sprom);
	if (err)
		goto out;

	bcma_sprom_extract_r8(bus, sprom);

out:
	kfree(sprom);
	return err;
}
