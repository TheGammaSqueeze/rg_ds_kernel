/* drivers/input/touchscreen/gt9xx_cfg.h
 *
 * 2010 - 2013 Goodix Technology.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be a reference
 * to you, when you are integrating the GOODiX's CTP IC into your system,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 */

#ifndef _GOODIX_GT9XX_CFG_H_
#define _GOODIX_GT9XX_CFG_H_

/*
 * RG DS Plus (1024x768 GT911). Read out of the panel vendor's own firmware and
 * confirmed byte for byte against a working unit's controller: config version
 * 0x5b, 1024x768, 5 touch points, 15+26 drive and 15 sense channels.
 *
 * The generic Goodix sample below (gtp_dat_gt11) describes a 24 drive / 5 sense
 * tablet digitiser at 4096x4096. Writing that over this panel's calibration
 * leaves touches landing short of where they are made, progressively worse
 * toward the far edge. Units whose controller reports a config version of 90 or
 * more are never written to and so were never affected; the ones below 90 were.
 */
static u8 gtp_dat_gt911_1024x768[] = {
	#include "RGDSPLUS_GT911_1024x768_V5B.cfg"
};

/* CFG for GT911 */
static u8 gtp_dat_gt11[] = {
	/* <1200, 1920>*/
	#include "WGJ89006B_GT911_Config_20140625_085816_0X43.cfg"
};

static u8 gtp_dat_gt9110[] = {
	/* <1200, 1920>*/
	#include "GT9110P(2020)V71_Config_20201028_170326.cfg"
};

static u8 gtp_dat_gt9111[] = {
	#include "HLS-0102-1398V1-1060-GT911_Config_20201204_V66.cfg"
};

static u8 gtp_dat_8_9[] = {
	/* TODO:Puts your update firmware data here! */
	/* <1920, 1200> 8.9 */
	/* #include "WGJ89006B_GT9271_Config_20140625_085816_0X41.cfg" */
	/* #include "WGJ10162_GT9271_Config_20140820_182456.cfg" */
	#include "WGJ10162B_GT9271_1060_Config_20140821_1341110X42.cfg"
};

static u8 gtp_dat_8_9_1[] = {
	#include "GT9271_Config_20170526.cfg"
};

static u8 gtp_dat_9_7[] = {
	/* <1536, 2048> 9.7 */
	#include "GT9110P_Config_20160217_1526_2048_97.cfg"
};

static u8 gtp_dat_10_1[] = {
	/* TODO:Puts your update firmware data here! */
	/* <1200, 1920> 10.1 */
	#include "WGJ10187_GT9271_Config_20140623_104014_0X41.cfg"
};

static u8 gtp_dat_7[] = {
	/* TODO:Puts your update firmware data here! */
	/* <1024, 600> 7.0 */
	#include "WGJ10187_GT910_Config_20140623_104014_0X41.cfg"
};

#endif /* _GOODIX_GT9XX_CFG_H_ */
