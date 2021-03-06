/*
*
* drivers/media/tdmb/tdmb_port_mtv319.c
*
* tdmb driver
*
* Copyright (C) (2013, Samsung Electronics)
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation version 2.
*
* This program is distributed "as is" WITHOUT ANY WARRANTY of any
* kind, whether express or implied; without even the implied warranty
* of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
*/

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/fcntl.h>

#include <linux/workqueue.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/cache.h>

#include <linux/fs.h>
#include <linux/uaccess.h>

#include <linux/time.h>
#include <linux/timer.h>

#include <linux/vmalloc.h>

#include "mtv319.h"
#include "mtv319_internal.h"
#include "mtv319_ficdec_internal.h"
#include "mtv319_ficdec.h"

#include "tdmb.h"

#define MTV319_INTERRUPT_SIZE (188*16)
#define MTV319_TS_BUF_SIZE (MTV319_SPI_CMD_SIZE + MTV319_INTERRUPT_SIZE)

/* #define TDMB_DEBUG_SCAN */

static bool mtv319_on_air;
static bool mtv319_pwr_on;
static U8 stream_buff[MTV319_TS_BUF_SIZE] __cacheline_aligned;
static U8 fic_buf[MTV319_FIC_BUF_SIZE] __cacheline_aligned;

#ifdef TDMB_DEBUG_SCAN
static void __print_ensemble_info(struct ensemble_info_type *e_info)
{
	int i = 0;

	DPRINTK("ensem_freq(%ld)\n", e_info->ensem_freq);
	DPRINTK("ensem_label(%s)\n", e_info->ensem_label);
	for (i = 0; i < e_info->tot_sub_ch; i++) {
		DPRINTK("sub_ch_id(0x%x)\n", e_info->sub_ch[i].sub_ch_id);
		DPRINTK("start_addr(0x%x)\n", e_info->sub_ch[i].start_addr);
		DPRINTK("tmid(0x%x)\n", e_info->sub_ch[i].tmid);
		DPRINTK("svc_type(0x%x)\n", e_info->sub_ch[i].svc_type);
		DPRINTK("svc_label(%s)\n", e_info->sub_ch[i].svc_label);
	}
}
#endif

static bool __get_ensemble_info(struct ensemble_info_type *e_info
						, unsigned long freq)
{
	DPRINTK("%s\n", __func__);

	rtvFICDEC_GetEnsembleInfo(e_info, freq);

	if (e_info->tot_sub_ch) {
#ifdef TDMB_DEBUG_SCAN
		__print_ensemble_info(e_info);
#endif
		return true;
	} else {
		return false;
	}
}

static void mtv319_power_off(void)
{
	DPRINTK("mtv319_power_off\n");

	if (mtv319_pwr_on) {
		mtv319_on_air = false;
		tdmb_control_irq(false);
		tdmb_control_gpio(false);

		mtv319_pwr_on = false;

		RTV_GUARD_DEINIT;
	}
}

static bool mtv319_power_on(void)
{
	DPRINTK("mtv319_power_on\n");

	if (mtv319_pwr_on) {
		return true;
	} else {
		tdmb_control_gpio(true);

		RTV_GUARD_INIT;

		if (rtvTDMB_Initialize() != RTV_SUCCESS) {
			tdmb_control_gpio(false);
			return false;
		} else {
			tdmb_control_irq(true);
			mtv319_pwr_on = true;
			return true;
		}
	}
}

static void mtv319_get_dm(struct tdmb_dm *info)
{
	if (mtv319_pwr_on == true && mtv319_on_air == true) {
		info->rssi = (rtvTDMB_GetRSSI() / RTV_TDMB_RSSI_DIVIDER);
		info->per = rtvTDMB_GetPER();
		info->ber = rtvTDMB_GetCER();
		info->antenna = rtvTDMB_GetAntennaLevel(info->ber);
	} else {
		info->rssi = 100;
		info->ber = 2000;
		info->per = 0;
		info->antenna = 0;
	}
}

static bool mtv319_set_ch(unsigned long freq
						, unsigned char subchid
						, bool factory_test)
{
	bool ret = false;
	enum E_RTV_SERVICE_TYPE svc_type;

	DPRINTK("%s : %ld %d\n", __func__, freq, subchid);

	if (mtv319_pwr_on) {
		int ch_ret;

		rtvTDMB_CloseAllSubChannels();

		if (subchid >= 64) {
			subchid -= 64;
			svc_type = RTV_SERVICE_DMB;
		} else
			svc_type = RTV_SERVICE_DAB;

		ch_ret = rtvTDMB_OpenSubChannel((freq/1000),
							subchid,
							svc_type,
							MTV319_INTERRUPT_SIZE);
		if (ch_ret == RTV_SUCCESS
		|| ch_ret == RTV_ALREADY_OPENED_SUBCHANNEL_ID) {
			mtv319_on_air = true;
			ret = TRUE;
			DPRINTK("mtv319_set_ch Success\n");
		} else {
			DPRINTK("mtv319_set_ch Fail (%d)\n", ch_ret);
		}
	}

	return ret;
}

static bool mtv319_scan_ch(struct ensemble_info_type *e_info
							, unsigned long freq)
{
	enum E_RTV_FIC_DEC_RET_TYPE dc;
	bool ret = false;

	if (mtv319_pwr_on == true && e_info != NULL) {
		rtvTDMB_CloseAllSubChannels();

		if (rtvTDMB_ScanFrequency(freq/1000) == RTV_SUCCESS) {
			unsigned int i;
			int ret_size;

			rtvFICDEC_Init(); /* FIC parser Init */

			for (i = 0; i < 40; i++) {
				ret_size = rtvTDMB_ReadFIC(fic_buf);
				if (ret_size > 0) {
					dc = rtvFICDEC_Decode(fic_buf, 384);
					if (dc == RTV_FIC_RET_GOING)
						continue;

					if (dc == RTV_FIC_RET_DONE)
						ret = true;

					break; /* Stop */
				} else {
					DPRINTK("mtv319_scan_ch READ Fail\n");
				}
			}

			rtvTDMB_CloseFIC();
			if (ret == true)
				__get_ensemble_info(e_info, (freq));
		}
	}

	return ret;
}

static void mtv319_pull_data(void)
{
	U8 ifreg, istatus;

	RTV_GUARD_LOCK;

	RTV_REG_MAP_SEL(SPI_CTRL_PAGE);

	ifreg = RTV_REG_GET(0x55);
	if (ifreg != 0xAA) {
		mtv319_spi_recover(stream_buff, MTV319_INTERRUPT_SIZE);
		DPRINTK("Interface error 1\n");
	}

	istatus = RTV_REG_GET(0x10);
	if (istatus & (U8)(~SPI_INTR_BITS)) {
		mtv319_spi_recover(stream_buff, MTV319_INTERRUPT_SIZE);
		DPRINTK("Interface error 2 (0x%02X)\n", istatus);
		goto exit_read_mem;
	}

	if (istatus & SPI_UNDERFLOW_INTR) {
		RTV_REG_SET(0x2A, 1);
		RTV_REG_SET(0x2A, 0);
		DPRINTK("UDF: 0x%02X\n", istatus);
		goto exit_read_mem;
	}

	if (istatus & SPI_THRESHOLD_INTR) {
		RTV_REG_MAP_SEL(SPI_MEM_PAGE);
		RTV_REG_BURST_GET(0x10, stream_buff, MTV319_INTERRUPT_SIZE);

		tdmb_store_data(stream_buff, MTV319_INTERRUPT_SIZE);

		if (istatus & SPI_OVERFLOW_INTR)
			DPRINTK("OVF: 0x%02X\n", istatus); /* To debug */
	} else
		DPRINTK("No data interrupt (0x%02X)\n", istatus);

exit_read_mem:
	RTV_GUARD_FREE;
}

static unsigned long mtv319_int_size(void)
{
	return MTV319_INTERRUPT_SIZE;
}

static bool mtv319_init(void)
{
	return true;
}

static struct tdmb_drv_func raontech_mtv319_drv_func = {
	.init = mtv319_init,
	.power_on = mtv319_power_on,
	.power_off = mtv319_power_off,
	.scan_ch = mtv319_scan_ch,
	.get_dm = mtv319_get_dm,
	.set_ch = mtv319_set_ch,
	.pull_data = mtv319_pull_data,
	.get_int_size = mtv319_int_size,
};

struct tdmb_drv_func *mtv319_drv_func(void)
{
	DPRINTK("tdmb_get_drv_func : mtv319\n");
	return &raontech_mtv319_drv_func;
}
