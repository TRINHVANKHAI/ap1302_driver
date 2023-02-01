// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2011-2013 Freescale Semiconductor, Inc. All Rights Reserved.
 * Copyright (C) 2014-2017 Mentor Graphics Inc.
 * Copyright (C) 2020 Hinoeng.
 */

#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/clkdev.h>
#include <linux/ctype.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/firmware.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <media/v4l2-async.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-event.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* min/typical/max system clock (xclk) frequencies */
#define AP1302_XCLK_MIN  6000000
#define AP1302_XCLK_MAX 54000000

#define DRIVER_NAME "ap1302"

#define AP1302_FW_WINDOW_SIZE            0x2000
#define AP1302_FW_WINDOW_OFFSET            0x8000
#define AP1302_FW_BLOCK_LEN         0x800
#define AP1302_MIN_WIDTH            24U
#define AP1302_MIN_HEIGHT            16U
#define AP1302_MAX_WIDTH            4224U
#define AP1302_MAX_HEIGHT            4092U

#define AP1302_REG_16BIT(n)            ((2 << 24) | (n))
#define AP1302_REG_32BIT(n)            ((4 << 24) | (n))
#define AP1302_REG_SIZE(n)            ((n) >> 24)
#define AP1302_REG_ADDR(n)            ((n) & 0x0000ffff)
#define AP1302_REG_PAGE(n)            ((n) & 0x00ff0000)
#define AP1302_REG_PAGE_MASK            0x00ff0000

/* Info Registers */
#define AP1302_CHIP_VERSION            AP1302_REG_16BIT(0x0000)
#define AP1302_CHIP_ID                0x0265
#define AP1302_FRAME_CNT            AP1302_REG_16BIT(0x0002)
#define AP1302_ERROR                AP1302_REG_16BIT(0x0006)
#define AP1302_ERR_FILE                AP1302_REG_32BIT(0x0008)
#define AP1302_ERR_LINE                AP1302_REG_16BIT(0x000c)
#define AP1302_SIPM_ERR_0            AP1302_REG_16BIT(0x0014)
#define AP1302_SIPM_ERR_1            AP1302_REG_16BIT(0x0016)
#define AP1302_CHIP_REV                AP1302_REG_16BIT(0x0050)
#define AP1302_CON_BUF(n)            AP1302_REG_16BIT(0x0a2c + (n))
#define AP1302_CON_BUF_SIZE            512

/* Control Registers */
#define AP1302_DZ_TGT_FCT            AP1302_REG_16BIT(0x1010)
#define AP1302_SFX_MODE                AP1302_REG_16BIT(0x1016)
#define AP1302_SFX_MODE_SFX_NORMAL        (0U << 0)
#define AP1302_SFX_MODE_SFX_ALIEN        (1U << 0)
#define AP1302_SFX_MODE_SFX_ANTIQUE        (2U << 0)
#define AP1302_SFX_MODE_SFX_BW            (3U << 0)
#define AP1302_SFX_MODE_SFX_EMBOSS        (4U << 0)
#define AP1302_SFX_MODE_SFX_EMBOSS_COLORED    (5U << 0)
#define AP1302_SFX_MODE_SFX_GRAYSCALE        (6U << 0)
#define AP1302_SFX_MODE_SFX_NEGATIVE        (7U << 0)
#define AP1302_SFX_MODE_SFX_BLUISH        (8U << 0)
#define AP1302_SFX_MODE_SFX_GREENISH        (9U << 0)
#define AP1302_SFX_MODE_SFX_REDISH        (10U << 0)
#define AP1302_SFX_MODE_SFX_POSTERIZE1        (11U << 0)
#define AP1302_SFX_MODE_SFX_POSTERIZE2        (12U << 0)
#define AP1302_SFX_MODE_SFX_SEPIA1        (13U << 0)
#define AP1302_SFX_MODE_SFX_SEPIA2        (14U << 0)
#define AP1302_SFX_MODE_SFX_SKETCH        (15U << 0)
#define AP1302_SFX_MODE_SFX_SOLARIZE        (16U << 0)
#define AP1302_SFX_MODE_SFX_FOGGY        (17U << 0)
#define AP1302_BUBBLE_OUT_FMT            AP1302_REG_16BIT(0x1164)
#define AP1302_BUBBLE_OUT_FMT_FT_YUV        (3U << 4)
#define AP1302_BUBBLE_OUT_FMT_FT_RGB        (4U << 4)
#define AP1302_BUBBLE_OUT_FMT_FT_YUV_JFIF    (5U << 4)
#define AP1302_BUBBLE_OUT_FMT_FST_RGB_888    (0U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_RGB_565    (1U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_RGB_555M    (2U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_RGB_555L    (3U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_YUV_422    (0U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_YUV_420    (1U << 0)
#define AP1302_BUBBLE_OUT_FMT_FST_YUV_400    (2U << 0)
#define AP1302_ATOMIC                AP1302_REG_16BIT(0x1184)
#define AP1302_ATOMIC_MODE            BIT(2)
#define AP1302_ATOMIC_FINISH            BIT(1)
#define AP1302_ATOMIC_RECORD            BIT(0)

/*
 * Preview Context Registers (preview_*). AP1302 supports 3 "contexts"
 * (Preview, Snapshot, Video). These can be programmed for different size,
 * format, FPS, etc. There is no functional difference between the contexts,
 * so the only potential benefit of using them is reduced number of register
 * writes when switching output modes (if your concern is atomicity, see
 * "atomic" register).
 * So there's virtually no benefit in using contexts for this driver and it
 * would significantly increase complexity. Let's use preview context only.
 */
#define AP1302_PREVIEW_WIDTH            AP1302_REG_16BIT(0x2000)
#define AP1302_PREVIEW_HEIGHT            AP1302_REG_16BIT(0x2002)
#define AP1302_PREVIEW_ROI_X0            AP1302_REG_16BIT(0x2004)
#define AP1302_PREVIEW_ROI_Y0            AP1302_REG_16BIT(0x2006)
#define AP1302_PREVIEW_ROI_X1            AP1302_REG_16BIT(0x2008)
#define AP1302_PREVIEW_ROI_Y1            AP1302_REG_16BIT(0x200a)
#define AP1302_PREVIEW_OUT_FMT            AP1302_REG_16BIT(0x2012)
#define AP1302_PREVIEW_OUT_FMT_IPIPE_BYPASS    BIT(13)
#define AP1302_PREVIEW_OUT_FMT_SS        BIT(12)
#define AP1302_PREVIEW_OUT_FMT_FAKE_EN        BIT(11)
#define AP1302_PREVIEW_OUT_FMT_ST_EN        BIT(10)
#define AP1302_PREVIEW_OUT_FMT_IIS_NONE        (0U << 8)
#define AP1302_PREVIEW_OUT_FMT_IIS_POST_VIEW    (1U << 8)
#define AP1302_PREVIEW_OUT_FMT_IIS_VIDEO    (2U << 8)
#define AP1302_PREVIEW_OUT_FMT_IIS_BUBBLE    (3U << 8)
#define AP1302_PREVIEW_OUT_FMT_FT_JPEG_422    (0U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_JPEG_420    (1U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_YUV        (3U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RGB        (4U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_YUV_JFIF    (5U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RAW8        (8U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RAW10        (9U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RAW12        (10U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_RAW16        (11U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_DNG8        (12U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_DNG10        (13U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_DNG12        (14U << 4)
#define AP1302_PREVIEW_OUT_FMT_FT_DNG16        (15U << 4)
#define AP1302_PREVIEW_OUT_FMT_FST_JPEG_ROTATE    BIT(2)
#define AP1302_PREVIEW_OUT_FMT_FST_JPEG_SCAN    (0U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_JPEG_JFIF    (1U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_JPEG_EXIF    (2U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RGB_888    (0U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RGB_565    (1U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RGB_555M    (2U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RGB_555L    (3U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_YUV_422    (0U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_YUV_420    (1U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_YUV_400    (2U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_SENSOR    (0U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_CAPTURE    (1U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_CP    (2U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_BPC    (3U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_IHDR    (4U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_PP    (5U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_DENSH    (6U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_PM    (7U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_GC    (8U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_CURVE    (9U << 0)
#define AP1302_PREVIEW_OUT_FMT_FST_RAW_CCONV    (10U << 0)
#define AP1302_PREVIEW_S1_SENSOR_MODE        AP1302_REG_16BIT(0x202e)
#define AP1302_PREVIEW_HINF_CTRL        AP1302_REG_16BIT(0x2030)
#define AP1302_PREVIEW_HINF_CTRL_BT656_LE    BIT(15)
#define AP1302_PREVIEW_HINF_CTRL_BT656_16BIT    BIT(14)
#define AP1302_PREVIEW_HINF_CTRL_MUX_DELAY(n)    ((n) << 8)
#define AP1302_PREVIEW_HINF_CTRL_LV_POL        BIT(7)
#define AP1302_PREVIEW_HINF_CTRL_FV_POL        BIT(6)
#define AP1302_PREVIEW_HINF_CTRL_MIPI_CONT_CLK    BIT(5)
#define AP1302_PREVIEW_HINF_CTRL_SPOOF        BIT(4)
#define AP1302_PREVIEW_HINF_CTRL_MIPI_MODE    BIT(3)
#define AP1302_PREVIEW_HINF_CTRL_MIPI_LANES(n)    ((n) << 0)

/* IQ Registers */
#define AP1302_AE_CTRL            AP1302_REG_16BIT(0x5002)
#define AP1302_AE_CTRL_STATS_SEL        BIT(11)
#define AP1302_AE_CTRL_IMM                BIT(10)
#define AP1302_AE_CTRL_ROUND_ISO        BIT(9)
#define AP1302_AE_CTRL_UROI_FACE        BIT(7)
#define AP1302_AE_CTRL_UROI_LOCK        BIT(6)
#define AP1302_AE_CTRL_UROI_BOUND        BIT(5)
#define AP1302_AE_CTRL_IMM1                BIT(4)
#define AP1302_AE_CTRL_MANUAL_EXP_TIME_GAIN    (0U << 0)
#define AP1302_AE_CTRL_MANUAL_BV_EXP_TIME    (1U << 0)
#define AP1302_AE_CTRL_MANUAL_BV_GAIN        (2U << 0)
#define AP1302_AE_CTRL_MANUAL_BV_ISO        (3U << 0)
#define AP1302_AE_CTRL_AUTO_BV_EXP_TIME        (9U << 0)
#define AP1302_AE_CTRL_AUTO_BV_GAIN            (10U << 0)
#define AP1302_AE_CTRL_AUTO_BV_ISO            (11U << 0)
#define AP1302_AE_CTRL_FULL_AUTO            (12U << 0)
#define AP1302_AE_CTRL_MODE_MASK        0x000f
#define AP1302_AE_MANUAL_GAIN        AP1302_REG_16BIT(0x5006)
#define AP1302_AE_BV_OFF            AP1302_REG_16BIT(0x5014)
#define AP1302_AE_MET                AP1302_REG_16BIT(0x503E)
#define AP1302_AWB_CTRL                AP1302_REG_16BIT(0x5100)
#define AP1302_AWB_CTRL_RECALC            BIT(13)
#define AP1302_AWB_CTRL_POSTGAIN        BIT(12)
#define AP1302_AWB_CTRL_UNGAIN            BIT(11)
#define AP1302_AWB_CTRL_CLIP            BIT(10)
#define AP1302_AWB_CTRL_SKY            BIT(9)
#define AP1302_AWB_CTRL_FLASH            BIT(8)
#define AP1302_AWB_CTRL_FACE_OFF        (0U << 6)
#define AP1302_AWB_CTRL_FACE_IGNORE        (1U << 6)
#define AP1302_AWB_CTRL_FACE_CONSTRAINED    (2U << 6)
#define AP1302_AWB_CTRL_FACE_ONLY        (3U << 6)
#define AP1302_AWB_CTRL_IMM            BIT(5)
#define AP1302_AWB_CTRL_IMM1            BIT(4)
#define AP1302_AWB_CTRL_MODE_OFF        (0U << 0)
#define AP1302_AWB_CTRL_MODE_HORIZON        (1U << 0)
#define AP1302_AWB_CTRL_MODE_A            (2U << 0)
#define AP1302_AWB_CTRL_MODE_CWF        (3U << 0)
#define AP1302_AWB_CTRL_MODE_D50        (4U << 0)
#define AP1302_AWB_CTRL_MODE_D65        (5U << 0)
#define AP1302_AWB_CTRL_MODE_D75        (6U << 0)
#define AP1302_AWB_CTRL_MODE_MANUAL        (7U << 0)
#define AP1302_AWB_CTRL_MODE_MEASURE        (8U << 0)
#define AP1302_AWB_CTRL_MODE_AUTO        (15U << 0)
#define AP1302_AWB_CTRL_MODE_MASK        0x000f
#define AP1302_FLICK_CTRL            AP1302_REG_16BIT(0x5440)
#define AP1302_FLICK_CTRL_FREQ(n)        ((n) << 8)
#define AP1302_FLICK_CTRL_ETC_IHDR_UP        BIT(6)
#define AP1302_FLICK_CTRL_ETC_DIS        BIT(5)
#define AP1302_FLICK_CTRL_FRC_OVERRIDE_MAX_ET    BIT(4)
#define AP1302_FLICK_CTRL_FRC_OVERRIDE_UPPER_ET    BIT(3)
#define AP1302_FLICK_CTRL_FRC_EN        BIT(2)
#define AP1302_FLICK_CTRL_MODE_DISABLED        (0U << 0)
#define AP1302_FLICK_CTRL_MODE_MANUAL        (1U << 0)
#define AP1302_FLICK_CTRL_MODE_AUTO        (2U << 0)
#define AP1302_SCENE_CTRL            AP1302_REG_16BIT(0x5454)
#define AP1302_SCENE_CTRL_MODE_NORMAL        (0U << 0)
#define AP1302_SCENE_CTRL_MODE_PORTRAIT        (1U << 0)
#define AP1302_SCENE_CTRL_MODE_LANDSCAPE    (2U << 0)
#define AP1302_SCENE_CTRL_MODE_SPORT        (3U << 0)
#define AP1302_SCENE_CTRL_MODE_CLOSE_UP        (4U << 0)
#define AP1302_SCENE_CTRL_MODE_NIGHT        (5U << 0)
#define AP1302_SCENE_CTRL_MODE_TWILIGHT        (6U << 0)
#define AP1302_SCENE_CTRL_MODE_BACKLIGHT    (7U << 0)
#define AP1302_SCENE_CTRL_MODE_HIGH_SENSITIVE    (8U << 0)
#define AP1302_SCENE_CTRL_MODE_NIGHT_PORTRAIT    (9U << 0)
#define AP1302_SCENE_CTRL_MODE_BEACH        (10U << 0)
#define AP1302_SCENE_CTRL_MODE_DOCUMENT        (11U << 0)
#define AP1302_SCENE_CTRL_MODE_PARTY        (12U << 0)
#define AP1302_SCENE_CTRL_MODE_FIREWORKS    (13U << 0)
#define AP1302_SCENE_CTRL_MODE_SUNSET        (14U << 0)
#define AP1302_SCENE_CTRL_MODE_AUTO        (0xffU << 0)

/* System Registers */
#define AP1302_BOOTDATA_STAGE            AP1302_REG_16BIT(0x6002)
#define AP1302_WARNING(n)            AP1302_REG_16BIT(0x6004 + (n) * 2)
#define AP1302_SENSOR_SELECT            AP1302_REG_16BIT(0x600c)
#define AP1302_SENSOR_SELECT_TP_MODE(n)        ((n) << 8)
#define AP1302_SENSOR_SELECT_PATTERN_ON        BIT(7)
#define AP1302_SENSOR_SELECT_MODE_3D_ON        BIT(6)
#define AP1302_SENSOR_SELECT_CLOCK        BIT(5)
#define AP1302_SENSOR_SELECT_SINF_MIPI        BIT(4)
#define AP1302_SENSOR_SELECT_YUV        BIT(2)
#define AP1302_SENSOR_SELECT_SENSOR_TP        (0U << 0)
#define AP1302_SENSOR_SELECT_SENSOR(n)        (((n) + 1) << 0)
#define AP1302_SYS_START            AP1302_REG_16BIT(0x601a)
#define AP1302_SYS_START_PLL_LOCK        BIT(15)
#define AP1302_SYS_START_LOAD_OTP        BIT(12)
#define AP1302_SYS_START_RESTART_ERROR        BIT(11)
#define AP1302_SYS_START_STALL_STATUS        BIT(9)
#define AP1302_SYS_START_STALL_EN        BIT(8)
#define AP1302_SYS_START_STALL_MODE_FRAME    (0U << 6)
#define AP1302_SYS_START_STALL_MODE_DISABLED    (1U << 6)
#define AP1302_SYS_START_STALL_MODE_POWER_DOWN    (2U << 6)
#define AP1302_SYS_START_GO            BIT(4)
#define AP1302_SYS_START_PATCH_FUN        BIT(1)
#define AP1302_SYS_START_PLL_INIT        BIT(0)
#define AP1302_DMA_SRC                AP1302_REG_32BIT(0x60a0)
#define AP1302_DMA_DST                AP1302_REG_32BIT(0x60a4)
#define AP1302_DMA_SIP_SIPM(n)            ((n) << 26)
#define AP1302_DMA_SIP_DATA_16_BIT        BIT(25)
#define AP1302_DMA_SIP_ADDR_16_BIT        BIT(24)
#define AP1302_DMA_SIP_ID(n)            ((n) << 17)
#define AP1302_DMA_SIP_REG(n)            ((n) << 0)
#define AP1302_DMA_SIZE                AP1302_REG_32BIT(0x60a8)
#define AP1302_DMA_CTRL                AP1302_REG_16BIT(0x60ac)
#define AP1302_DMA_CTRL_SCH_NORMAL        (0 << 12)
#define AP1302_DMA_CTRL_SCH_NEXT        (1 << 12)
#define AP1302_DMA_CTRL_SCH_NOW            (2 << 12)
#define AP1302_DMA_CTRL_DST_REG            (0 << 8)
#define AP1302_DMA_CTRL_DST_SRAM        (1 << 8)
#define AP1302_DMA_CTRL_DST_SPI            (2 << 8)
#define AP1302_DMA_CTRL_DST_SIP            (3 << 8)
#define AP1302_DMA_CTRL_SRC_REG            (0 << 4)
#define AP1302_DMA_CTRL_SRC_SRAM        (1 << 4)
#define AP1302_DMA_CTRL_SRC_SPI            (2 << 4)
#define AP1302_DMA_CTRL_SRC_SIP            (3 << 4)
#define AP1302_DMA_CTRL_MODE_32_BIT        BIT(3)
#define AP1302_DMA_CTRL_MODE_MASK        (7 << 0)
#define AP1302_DMA_CTRL_MODE_IDLE        (0 << 0)
#define AP1302_DMA_CTRL_MODE_SET        (1 << 0)
#define AP1302_DMA_CTRL_MODE_COPY        (2 << 0)
#define AP1302_DMA_CTRL_MODE_MAP        (3 << 0)
#define AP1302_DMA_CTRL_MODE_UNPACK        (4 << 0)
#define AP1302_DMA_CTRL_MODE_OTP_READ        (5 << 0)
#define AP1302_DMA_CTRL_MODE_SIP_PROBE        (6 << 0)

#define AP1302_BRIGHTNESS            AP1302_REG_16BIT(0x7000)
#define AP1302_CONTRAST            AP1302_REG_16BIT(0x7002)
#define AP1302_SATURATION            AP1302_REG_16BIT(0x7006)
#define AP1302_GAMMA                AP1302_REG_16BIT(0x700A)

/* Misc Registers */
#define AP1302_REG_ADV_START            0xe000
#define AP1302_ADVANCED_BASE            AP1302_REG_32BIT(0xf038)
#define AP1302_SIP_CRC                AP1302_REG_16BIT(0xf052)
#define AP1302_SIP_CHECKSUM           AP1302_REG_16BIT(0x6134)

/* Advanced System Registers */
#define AP1302_ADV_IRQ_SYS_INTE            AP1302_REG_32BIT(0x00230000)
#define AP1302_ADV_IRQ_SYS_INTE_TEST_COUNT    BIT(25)
#define AP1302_ADV_IRQ_SYS_INTE_HINF_1        BIT(24)
#define AP1302_ADV_IRQ_SYS_INTE_HINF_0        BIT(23)
#define AP1302_ADV_IRQ_SYS_INTE_SINF_B_MIPI_L    (7U << 20)
#define AP1302_ADV_IRQ_SYS_INTE_SINF_B_MIPI    BIT(19)
#define AP1302_ADV_IRQ_SYS_INTE_SINF_A_MIPI_L    (15U << 14)
#define AP1302_ADV_IRQ_SYS_INTE_SINF_A_MIPI    BIT(13)
#define AP1302_ADV_IRQ_SYS_INTE_SINF        BIT(12)
#define AP1302_ADV_IRQ_SYS_INTE_IPIPE_S        BIT(11)
#define AP1302_ADV_IRQ_SYS_INTE_IPIPE_B        BIT(10)
#define AP1302_ADV_IRQ_SYS_INTE_IPIPE_A        BIT(9)
#define AP1302_ADV_IRQ_SYS_INTE_IP        BIT(8)
#define AP1302_ADV_IRQ_SYS_INTE_TIMER        BIT(7)
#define AP1302_ADV_IRQ_SYS_INTE_SIPM        (3U << 6)
#define AP1302_ADV_IRQ_SYS_INTE_SIPS_ADR_RANGE    BIT(5)
#define AP1302_ADV_IRQ_SYS_INTE_SIPS_DIRECT_WRITE    BIT(4)
#define AP1302_ADV_IRQ_SYS_INTE_SIPS_FIFO_WRITE    BIT(3)
#define AP1302_ADV_IRQ_SYS_INTE_SPI        BIT(2)
#define AP1302_ADV_IRQ_SYS_INTE_GPIO_CNT    BIT(1)
#define AP1302_ADV_IRQ_SYS_INTE_GPIO_PIN    BIT(0)

/* Advanced Slave MIPI Registers */
#define AP1302_ADV_SINF_MIPI_INTERNAL_p_LANE_n_STAT(p, n) \
    AP1302_REG_32BIT(0x00420008 + (p) * 0x50000 + (n) * 0x20)
#define AP1302_LANE_ERR_LP_VAL(n)        (((n) >> 30) & 3)
#define AP1302_LANE_ERR_STATE(n)        (((n) >> 24) & 0xf)
#define AP1302_LANE_ERR                BIT(18)
#define AP1302_LANE_ABORT            BIT(17)
#define AP1302_LANE_LP_VAL(n)            (((n) >> 6) & 3)
#define AP1302_LANE_STATE(n)            ((n) & 0xf)
#define AP1302_LANE_STATE_STOP_S        0x0
#define AP1302_LANE_STATE_HS_REQ_S        0x1
#define AP1302_LANE_STATE_LP_REQ_S        0x2
#define AP1302_LANE_STATE_HS_S            0x3
#define AP1302_LANE_STATE_LP_S            0x4
#define AP1302_LANE_STATE_ESC_REQ_S        0x5
#define AP1302_LANE_STATE_TURN_REQ_S        0x6
#define AP1302_LANE_STATE_ESC_S            0x7
#define AP1302_LANE_STATE_ESC_0            0x8
#define AP1302_LANE_STATE_ESC_1            0x9
#define AP1302_LANE_STATE_TURN_S        0xa
#define AP1302_LANE_STATE_TURN_MARK        0xb
#define AP1302_LANE_STATE_ERROR_S        0xc

#define AP1302_ADV_CAPTURE_A_FV_CNT        AP1302_REG_32BIT(0x00490040)
#define AP1302_ADV_HINF_MIPI_T3        AP1302_REG_32BIT(0x840014)
#define AP1302_TCLK_POST_MASK            0xFF
#define AP1302_TCLK_POST_SHIFT            0x0
#define AP1302_TCLK_PRE_MASK            0xFF00
#define AP1302_TCLK_PRE_SHIFT            0x8


enum ap1302_mode_id {
    AP1302_MODE_QCIF_176_144 = 0,
    AP1302_MODE_QVGA_320_240,
    AP1302_MODE_VGA_640_480,
    AP1302_MODE_NTSC_720_480,
    AP1302_MODE_PAL_720_576,
    AP1302_MODE_XGA_1024_768,
    AP1302_MODE_720P_1280_720,
    AP1302_MODE_1080P_1920_1080,
    AP1302_MODE_QSXGA_2592_1944,
    AP1302_MODE_4K_3840_2160,
    AP1302_NUM_MODES,
};

enum ap1302_frame_rate {
    AP1302_08_FPS = 0,
    AP1302_15_FPS,
    AP1302_30_FPS,
    AP1302_60_FPS,
    AP1302_NUM_FRAMERATES,
};

enum ap1302_format_mux {
    AP1302_FMT_MUX_YUV422 = 0,
    AP1302_FMT_MUX_RGB,
    AP1302_FMT_MUX_DITHER,
    AP1302_FMT_MUX_RAW_DPC,
    AP1302_FMT_MUX_SNR_RAW,
    AP1302_FMT_MUX_RAW_CIP,
};

struct ap1302_pixfmt {
    u32 code;
    u32 colorspace;
};

static const struct ap1302_pixfmt ap1302_formats[] = {
    { MEDIA_BUS_FMT_UYVY8_2X8, V4L2_COLORSPACE_SRGB,  },
    { MEDIA_BUS_FMT_UYVY8_1X16, V4L2_COLORSPACE_SRGB, },
    { MEDIA_BUS_FMT_YUYV8_2X8, V4L2_COLORSPACE_SRGB,  },
    { MEDIA_BUS_FMT_YUYV8_1X16, V4L2_COLORSPACE_SRGB, },
};

/*
 * FIXME: remove this when a subdev API becomes available
 * to set the MIPI CSI-2 virtual channel.
 */
static unsigned int virtual_channel;
module_param(virtual_channel, uint, 0444);
MODULE_PARM_DESC(virtual_channel,
         "MIPI CSI-2 virtual channel (0..3), default 0");

static const int ap1302_framerates[] = {
    [AP1302_08_FPS] = 8,
    [AP1302_15_FPS] = 15,
    [AP1302_30_FPS] = 30,
    [AP1302_60_FPS] = 60,
};

/* regulator supplies */
static const char * const ap1302_supply_name[] = {
    "DOVDD", /* Digital I/O (1.8V) supply */
    "AVDD",  /* Analog (2.8V) supply */
    "DVDD",  /* Digital Core (1.5V) supply */
};

#define AP1302_NUM_SUPPLIES ARRAY_SIZE(ap1302_supply_name)

struct ap1302_firmware_header {
    u16 pll_init_size;
    u16 crc;
} __packed;

#define MAX_FW_LOAD_RETRIES 3

/*
 * Image size under 1280 * 960 are SUBSAMPLING
 * Image size upper 1280 * 960 are SCALING
 */
enum ap1302_downsize_mode {
    SUBSAMPLING,
    SCALING,
};

struct reg_value {
    u32 reg_addr;
    u32 val;
    u32 mask;
    u32 delay_ms;
};

struct ap1302_mode_info {
    enum ap1302_mode_id id;
    enum ap1302_downsize_mode dn_mode;
    u32 hact;
    u32 htot;
    u32 vact;
    u32 vtot;
    const struct reg_value *reg_data;
    u32 reg_data_size;
    u32 max_fps;
};

struct ap1302_ctrls {
    struct v4l2_ctrl_handler handler;
    struct v4l2_ctrl *pixel_rate;
    struct {
        struct v4l2_ctrl *auto_exp;
        struct v4l2_ctrl *exposure;
    };
    struct {
        struct v4l2_ctrl *auto_wb;
        struct v4l2_ctrl *blue_balance;
        struct v4l2_ctrl *red_balance;
    };
    struct {
        struct v4l2_ctrl *auto_gain;
        struct v4l2_ctrl *gain;
    };
    struct v4l2_ctrl *brightness;
    struct v4l2_ctrl *light_freq;
    struct v4l2_ctrl *saturation;
    struct v4l2_ctrl *contrast;
    struct v4l2_ctrl *hue;
    struct v4l2_ctrl *test_pattern;
    struct v4l2_ctrl *hflip;
    struct v4l2_ctrl *vflip;
};

struct ap1302_dev {
    struct device *dev;
    struct i2c_client *i2c_client;
    struct v4l2_subdev sd;
    struct media_pad pad;
    struct v4l2_fwnode_endpoint ep; /* the parsed DT endpoint info */
    struct clk *xclk; /* system clock to AP1302 */
    u32 xclk_freq;
    struct regmap *regmap16;
    struct regmap *regmap32;
    u32 reg_page;

    const struct firmware *fw;
    const char *model;
    struct regulator_bulk_data supplies[AP1302_NUM_SUPPLIES];
    struct gpio_desc *reset_gpio;
    struct gpio_desc *pwdn_gpio;
    bool   upside_down;

    /* lock to protect all members below */
    struct mutex lock;

    int power_count;

    struct v4l2_mbus_framefmt fmt;
    bool pending_fmt_change;

    const struct ap1302_mode_info *current_mode;
    const struct ap1302_mode_info *last_mode;
    enum ap1302_frame_rate current_fr;
    struct v4l2_fract frame_interval;

    struct ap1302_ctrls ctrls;
    u32 prev_sysclk, prev_hts;
    u32 ae_low, ae_high, ae_target;

    bool pending_mode_change;
    bool streaming;
};


static inline struct ap1302_dev *to_ap1302_dev(struct v4l2_subdev *sd)
{
    return container_of(sd, struct ap1302_dev, sd);
}

static inline struct v4l2_subdev *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
    return &container_of(ctrl->handler, struct ap1302_dev,
                 ctrls.handler)->sd;
}

static const struct reg_value ap1302_init_setting_30fps_VGA[] = {

};

static const struct reg_value ap1302_setting_VGA_640_480[] = {

};

static const struct reg_value ap1302_setting_QVGA_320_240[] = {

};

static const struct reg_value ap1302_setting_QCIF_176_144[] = {

};

static const struct reg_value ap1302_setting_NTSC_720_480[] = {

};

static const struct reg_value ap1302_setting_PAL_720_576[] = {

};

static const struct reg_value ap1302_setting_XGA_1024_768[] = {

};

static const struct reg_value ap1302_setting_720P_1280_720[] = {

};

static const struct reg_value ap1302_setting_1080P_1920_1080[] = {

};

static const struct reg_value ap1302_setting_QSXGA_2592_1944[] = {

};

static const struct reg_value ap1302_setting_4K_3840_2160[] = {

};

/* power-on sensor init reg table */
static const struct ap1302_mode_info ap1302_mode_init_data = {
    AP1302_MODE_4K_3840_2160, SCALING,
    3840, 3840, 2160, 2160,
    ap1302_setting_4K_3840_2160,
    ARRAY_SIZE(ap1302_setting_4K_3840_2160),
    AP1302_30_FPS
};

static const struct ap1302_mode_info
ap1302_mode_data[AP1302_NUM_MODES] = {
    {AP1302_MODE_QCIF_176_144, SUBSAMPLING,
     176, 1896, 144, 984,
     ap1302_setting_QCIF_176_144,
     ARRAY_SIZE(ap1302_setting_QCIF_176_144),
     AP1302_30_FPS},
    {AP1302_MODE_QVGA_320_240, SUBSAMPLING,
     320, 1896, 240, 984,
     ap1302_setting_QVGA_320_240,
     ARRAY_SIZE(ap1302_setting_QVGA_320_240),
     AP1302_30_FPS},
    {AP1302_MODE_VGA_640_480, SUBSAMPLING,
     640, 1896, 480, 1080,
     ap1302_setting_VGA_640_480,
     ARRAY_SIZE(ap1302_setting_VGA_640_480),
     AP1302_30_FPS},
    {AP1302_MODE_NTSC_720_480, SUBSAMPLING,
     720, 1896, 480, 984,
     ap1302_setting_NTSC_720_480,
     ARRAY_SIZE(ap1302_setting_NTSC_720_480),
     AP1302_30_FPS},
    {AP1302_MODE_PAL_720_576, SUBSAMPLING,
     720, 1896, 576, 984,
     ap1302_setting_PAL_720_576,
     ARRAY_SIZE(ap1302_setting_PAL_720_576),
     AP1302_30_FPS},
    {AP1302_MODE_XGA_1024_768, SUBSAMPLING,
     1024, 1896, 768, 1080,
     ap1302_setting_XGA_1024_768,
     ARRAY_SIZE(ap1302_setting_XGA_1024_768),
     AP1302_30_FPS},
    {AP1302_MODE_720P_1280_720, SUBSAMPLING,
     1280, 1892, 720, 740,
     ap1302_setting_720P_1280_720,
     ARRAY_SIZE(ap1302_setting_720P_1280_720),
     AP1302_30_FPS},
    {AP1302_MODE_1080P_1920_1080, SCALING,
     1920, 2500, 1080, 1120,
     ap1302_setting_1080P_1920_1080,
     ARRAY_SIZE(ap1302_setting_1080P_1920_1080),
     AP1302_30_FPS},
    {AP1302_MODE_QSXGA_2592_1944, SCALING,
     2592, 2844, 1944, 1968,
     ap1302_setting_QSXGA_2592_1944,
     ARRAY_SIZE(ap1302_setting_QSXGA_2592_1944),
     AP1302_30_FPS},
    {AP1302_MODE_4K_3840_2160, SCALING,
     3840, 3840, 2160, 2160,
     ap1302_setting_4K_3840_2160,
     ARRAY_SIZE(ap1302_setting_4K_3840_2160),
     AP1302_30_FPS},
};

static int ap1302_init_slave_id(struct ap1302_dev *sensor)
{
    /*ID changing is not implemented */
    return 0;
}

/* -----------------------------------------------------------------------------
 * Register Configuration
 */
static const struct regmap_config ap1302_reg16_config = {
    .reg_bits = 16,
    .val_bits = 16,
    .reg_stride = 2,
    .reg_format_endian = REGMAP_ENDIAN_BIG,
    .val_format_endian = REGMAP_ENDIAN_BIG,
    .cache_type = REGCACHE_NONE,
};

static const struct regmap_config ap1302_reg32_config = {
    .reg_bits = 16,
    .val_bits = 32,
    .reg_stride = 4,
    .reg_format_endian = REGMAP_ENDIAN_BIG,
    .val_format_endian = REGMAP_ENDIAN_BIG,
    .cache_type = REGCACHE_NONE,
};

static int __ap1302_write(struct ap1302_dev *ap1302, u32 reg, u32 val)
{
    unsigned int size = AP1302_REG_SIZE(reg);
    u16 addr = AP1302_REG_ADDR(reg);
    int ret;

    switch (size) {
    case 2:
        ret = regmap_write(ap1302->regmap16, addr, val);
        break;
    case 4:
        ret = regmap_write(ap1302->regmap32, addr, val);
        break;
    default:
        return -EINVAL;
    }

    if (ret) {
        dev_err(ap1302->dev, "%s: register 0x%04x %s failed: %d\n",
                __func__, addr, "write", ret);
        return ret;
    }

    return 0;
}

static int ap1302_write(struct ap1302_dev *ap1302, u32 reg, u32 val,
            int *err)
{
    u32 page = AP1302_REG_PAGE(reg);
    int ret;

    if (err && *err)
        return *err;

    if (page) {
        if (ap1302->reg_page != page) {
            ret = __ap1302_write(ap1302, AP1302_ADVANCED_BASE, page);
            if (ret < 0)
                goto done;

            ap1302->reg_page = page;
        }

        reg &= ~AP1302_REG_PAGE_MASK;
        reg += AP1302_REG_ADV_START;
    }

    ret = __ap1302_write(ap1302, reg, val);

done:
    if (err && ret)
        *err = ret;

    return ret;
}

static int __ap1302_read(struct ap1302_dev *ap1302, u32 reg, u32 *val)
{
    unsigned int size = AP1302_REG_SIZE(reg);
    u16 addr = AP1302_REG_ADDR(reg);
    int ret;

    switch (size) {
    case 2:
        ret = regmap_read(ap1302->regmap16, addr, val);
        break;
    case 4:
        ret = regmap_read(ap1302->regmap32, addr, val);
        break;
    default:
        return -EINVAL;
    }

    if (ret) {
        dev_err(ap1302->dev, "%s: register 0x%04x %s failed: %d\n",
                __func__, addr, "read", ret);
                return ret;
    }
    
    dev_dbg(ap1302->dev, "%s: R0x%04x = 0x%0*x\n", __func__,
            addr, size * 2, *val);

    return 0;
}

static int ap1302_read(struct ap1302_dev *ap1302, u32 reg, u32 *val)
{
    u32 page = AP1302_REG_PAGE(reg);
    int ret;

    if (page) {
        if (ap1302->reg_page != page) {
            ret = __ap1302_write(ap1302, AP1302_ADVANCED_BASE, page);
            if (ret < 0)
                return ret;

            ap1302->reg_page = page;
        }

        reg &= ~AP1302_REG_PAGE_MASK;
        reg += AP1302_REG_ADV_START;
    }

    return __ap1302_read(ap1302, reg, val);
}

static int ap1302_write_reg16(struct ap1302_dev *sensor, u16 reg, u16 val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg;
	u8 buf[4];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;
	buf[2] = val >> 8;
	buf[3] = val & 0xff;

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.buf = buf;
	msg.len = sizeof(buf);

	ret = i2c_transfer(client->adapter, &msg, 1);
	if (ret < 0) {
		dev_err(&client->dev, "%s: error: reg=%x, val=%x\n",
			__func__, reg, val);
		return ret;
	}

	return 0;
}

static int ap1302_read_reg16(struct ap1302_dev *sensor, u16 reg, u16 *val)
{
	struct i2c_client *client = sensor->i2c_client;
	struct i2c_msg msg[2];
	u8 buf[2];
	int ret;

	buf[0] = reg >> 8;
	buf[1] = reg & 0xff;

	msg[0].addr = client->addr;
	msg[0].flags = client->flags;
	msg[0].buf = buf;
	msg[0].len = sizeof(buf);

	msg[1].addr = client->addr;
	msg[1].flags = client->flags | I2C_M_RD;
	msg[1].buf = buf;
	msg[1].len = sizeof(buf);

	ret = i2c_transfer(client->adapter, msg, 2);
	if (ret < 0) {
		dev_err(&client->dev, "%s: error: reg=%x\n",
			__func__, reg);
		return ret;
	}

	*val = (u16)buf[0] << 8 | buf[1];
	return 0;
}


static int ap1302_check_valid_mode(struct ap1302_dev *sensor,
                   const struct ap1302_mode_info *mode,
                   enum ap1302_frame_rate rate)
{
    int ret = 0;

    switch (mode->id) {
    case AP1302_MODE_QCIF_176_144:
    case AP1302_MODE_QVGA_320_240:
    case AP1302_MODE_NTSC_720_480:
    case AP1302_MODE_PAL_720_576 :
    case AP1302_MODE_XGA_1024_768:
    case AP1302_MODE_720P_1280_720:
        if ((rate != AP1302_15_FPS) &&
            (rate != AP1302_30_FPS))
            ret = -EINVAL;
        break;
    case AP1302_MODE_VGA_640_480:
        if ((rate != AP1302_15_FPS) &&
            (rate != AP1302_30_FPS))
            ret = -EINVAL;
        break;
    case AP1302_MODE_1080P_1920_1080:
        if (sensor->ep.bus_type == V4L2_MBUS_CSI2_DPHY) {
            if ((rate != AP1302_15_FPS) &&
                (rate != AP1302_30_FPS))
                ret = -EINVAL;
         } else {
            if ((rate != AP1302_15_FPS))
                ret = -EINVAL;
         }
        break;
    case AP1302_MODE_QSXGA_2592_1944:
        if (rate != AP1302_08_FPS)
            ret = -EINVAL;
        break;
    case AP1302_MODE_4K_3840_2160:
        if (rate != AP1302_30_FPS)
            ret = -EINVAL;
        break;
    default:
        dev_err(sensor->dev, "Invalid mode (%d)\n", mode->id);
        ret = -EINVAL;
    }

    return ret;
}

static int ap1302_load_regs(struct ap1302_dev *sensor,
                const struct ap1302_mode_info *mode)
{
    const struct reg_value *regs = mode->reg_data;
    unsigned int i;
    u32 delay_ms;
    u32 reg_addr;
    u32 mask, val;
    int ret = 0;

    for (i = 0; i < mode->reg_data_size; ++i, ++regs) {
        delay_ms = regs->delay_ms;
        reg_addr = regs->reg_addr;
        val = regs->val;
        mask = regs->mask;

        ret = ap1302_write(sensor, reg_addr, val, NULL);
        if (ret)
            break;

        if (delay_ms)
            usleep_range(1000 * delay_ms, 1000 * delay_ms + 100);
    }
    return ret;
}

static int ap1302_set_stream_dvp(struct ap1302_dev *sensor, bool on)
{
    int ret;
    unsigned int flags = sensor->ep.bus.parallel.flags;

    dev_warn(sensor->dev,"not supported\n");
    return -EINVAL;
}

static int ap1302_set_stream_mipi(struct ap1302_dev *sensor, bool on)
{
    int ret = 0;


    if (on) {
        ret = 0;
    } else {
        ret = 0;
    }

    return ret;
}

static int ap1302_set_virtual_channel(struct ap1302_dev *sensor)
{
    u8 temp, channel = virtual_channel;
    int ret;

    if (channel > 3) {
        dev_err(sensor->dev,
            "%s: wrong virtual_channel parameter, expected (0..3), got %d\n",
            __func__, channel);
        return -EINVAL;
    }
    return 0;
}

static const struct ap1302_mode_info *
ap1302_find_mode(struct ap1302_dev *sensor, enum ap1302_frame_rate fr,
         int width, int height, bool nearest)
{
    const struct ap1302_mode_info *mode;
    mode = v4l2_find_nearest_size(ap1302_mode_data,
                      ARRAY_SIZE(ap1302_mode_data),
                      hact, vact,
                      width, height);

    if (!mode ||
        (!nearest && (mode->hact != width || mode->vact != height))) {
        return NULL;
    }
    return mode;
}

static u64 ap1302_calc_pixel_rate(struct ap1302_dev *sensor)
{
    u64 rate;

    rate = sensor->current_mode->vtot * sensor->current_mode->htot;
    rate *= ap1302_framerates[sensor->current_fr];

    return rate;
}

/*
 * if sensor changes inside scaling or subsampling
 * change mode directly
 */
static int ap1302_set_mode_direct(struct ap1302_dev *sensor,
                  const struct ap1302_mode_info *mode)
{
    unsigned int data_lanes = sensor->ep.bus.mipi_csi2.num_data_lanes;
    int ret = 0;

    if (!mode->reg_data)
        return -EINVAL;

    /* Write capture setting */
    ap1302_write(sensor, AP1302_PREVIEW_HINF_CTRL,
                 AP1302_PREVIEW_HINF_CTRL_SPOOF |
                 AP1302_PREVIEW_HINF_CTRL_MIPI_LANES(data_lanes), &ret);

    ap1302_write(sensor, AP1302_PREVIEW_WIDTH,
                 mode->hact, &ret);
    ap1302_write(sensor, AP1302_PREVIEW_HEIGHT,
                 mode->vact, &ret);

    return ret;
}

static int ap1302_set_mode(struct ap1302_dev *sensor)
{
    const struct ap1302_mode_info *mode = sensor->current_mode;
    const struct ap1302_mode_info *orig_mode = sensor->last_mode;
    enum ap1302_downsize_mode dn_mode, orig_dn_mode;
    bool auto_gain = sensor->ctrls.auto_gain->val == 1;
    bool auto_exp =  sensor->ctrls.auto_exp->val == V4L2_EXPOSURE_AUTO;
    unsigned long rate;
    int ret;

    dn_mode = mode->dn_mode;
    orig_dn_mode = orig_mode->dn_mode;

    if ((dn_mode == SUBSAMPLING && orig_dn_mode == SCALING) ||
        (dn_mode == SCALING && orig_dn_mode == SUBSAMPLING)) {
        /*
         * change between subsampling and scaling
         * go through exposure calculation
         */
        ret = ap1302_set_mode_direct(sensor, mode);
    } else {
        /*
         * change inside subsampling or scaling
         * download firmware directly
         */
        ret = ap1302_set_mode_direct(sensor, mode);
    }

    sensor->pending_mode_change = false;
    sensor->last_mode = mode;

    return 0;
}

static int ap1302_set_framefmt(struct ap1302_dev *sensor,
                   struct v4l2_mbus_framefmt *format);

/* restore the last set video mode after chip power-on */
static int ap1302_restore_mode(struct ap1302_dev *sensor)
{
    int ret;

    /* first load the initial register values */
    ret = ap1302_load_regs(sensor, &ap1302_mode_init_data);
    if (ret < 0)
        return ret;
    sensor->last_mode = &ap1302_mode_init_data;

    /* now restore the last capture mode */
    ret = ap1302_set_mode(sensor);
    if (ret < 0)
        return ret;

    return ap1302_set_framefmt(sensor, &sensor->fmt);
}

static int ap1302_power_on(struct ap1302_dev *ap1302)
{
    int ret;

    /* 0. RESET was asserted when getting the GPIO. */

    /* 1. Assert STANDBY. */
    if (ap1302->pwdn_gpio) {
        gpiod_set_value_cansleep(ap1302->pwdn_gpio, 1);
        usleep_range(200, 1000);
    }

    /* 2. Power up the regulators. To be implemented. */

    /* 3. De-assert STANDBY. */
    if (ap1302->pwdn_gpio) {
        gpiod_set_value_cansleep(ap1302->pwdn_gpio, 0);
        usleep_range(200, 1000);
    }

    /* 5. De-assert RESET. */
    gpiod_set_value_cansleep(ap1302->reset_gpio, 0);

    /*
     * 6. Wait for the AP1302 to initialize. The datasheet doesn't specify
     * how long this takes.
     */
    usleep_range(10000, 11000);

    return 0;
}

static void ap1302_power_off(struct ap1302_dev *ap1302)
{
    /* 1. Assert RESET. */
    gpiod_set_value_cansleep(ap1302->reset_gpio, 1);

    /* 3. Assert STANDBY. */
    if (ap1302->pwdn_gpio) {
        gpiod_set_value_cansleep(ap1302->pwdn_gpio, 1);
        usleep_range(200, 1000);
    }

    /* 4. Power down the regulators. To be implemented. */

    /* 5. De-assert STANDBY. */
    if (ap1302->pwdn_gpio) {
        usleep_range(200, 1000);
        gpiod_set_value_cansleep(ap1302->pwdn_gpio, 0);
    }
}

static int ap1302_set_power_on(struct ap1302_dev *sensor)
{
    int ret;

    ret = clk_prepare_enable(sensor->xclk);
    if (ret) {
        dev_err(sensor->dev, "%s: failed to enable clock\n",
            __func__);
        return ret;
    }

    ret = regulator_bulk_enable(AP1302_NUM_SUPPLIES,
                    sensor->supplies);
    if (ret) {
        dev_err(sensor->dev, "%s: failed to enable regulators\n",
            __func__);
        goto xclk_off;
    }

    ap1302_power_on(sensor);
    ret = ap1302_init_slave_id(sensor);
    if (ret)
        goto power_off;

    return 0;

power_off:
    ap1302_power_off(sensor);
    regulator_bulk_disable(AP1302_NUM_SUPPLIES, sensor->supplies);
xclk_off:
    clk_disable_unprepare(sensor->xclk);
    return ret;
}

static void ap1302_set_power_off(struct ap1302_dev *sensor)
{
    ap1302_power_off(sensor);
    regulator_bulk_disable(AP1302_NUM_SUPPLIES, sensor->supplies);
    clk_disable_unprepare(sensor->xclk);
    sensor->streaming = false;
}

static int ap1302_set_powerdown_exit(struct ap1302_dev *sensor)
{
    return 0;
}

static int ap1302_set_powerdown_enter(struct ap1302_dev *sensor)
{
    
    sensor->streaming = false;
    return 0;
}

static int ap1302_set_power(struct ap1302_dev *sensor, bool on)
{
    int ret = 0;

    if (on) {
        ret = ap1302_set_powerdown_exit(sensor);
        if (ret)
            return ret;

        ret = ap1302_restore_mode(sensor);
        if (ret)
            goto power_off;

        /* We're done here for DVP bus, while CSI-2 needs setup. */
        if (sensor->ep.bus_type != V4L2_MBUS_CSI2_DPHY)
            return 0;
        usleep_range(500, 1000);

    } else {
        ret = ap1302_set_powerdown_enter(sensor);
    }

    return 0;

power_off:
    ap1302_set_powerdown_enter(sensor);
    return ret;
}


/* --------------- Subdev Operations --------------- */

static int ap1302_s_power(struct v4l2_subdev *sd, int on)
{
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    int ret = 0;

    mutex_lock(&sensor->lock);

    /*
     * If the power count is modified from 0 to != 0 or from != 0 to 0,
     * update the power state.
     */
    if (sensor->power_count == !on) {
        ret = ap1302_set_power(sensor, !!on);
        if (ret)
            goto out;
    }

    /* Update the power count. */
    sensor->power_count += on ? 1 : -1;
    WARN_ON(sensor->power_count < 0);
out:
    mutex_unlock(&sensor->lock);

    if (on && !ret && sensor->power_count == 1) {
        /* restore controls */
        
        ret = v4l2_ctrl_handler_setup(&sensor->ctrls.handler);
    }

    return ret;
}

static int ap1302_try_frame_interval(struct ap1302_dev *sensor,
                     struct v4l2_fract *fi,
                     u32 width, u32 height)
{
    const struct ap1302_mode_info *mode;
    enum ap1302_frame_rate rate = AP1302_08_FPS;
    int minfps, maxfps, best_fps, fps;
    int i;

    minfps = ap1302_framerates[AP1302_08_FPS];
    maxfps = ap1302_framerates[AP1302_60_FPS];

    if (fi->numerator == 0) {
        fi->denominator = maxfps;
        fi->numerator = 1;
        rate = AP1302_60_FPS;
        goto find_mode;
    }

    fps = clamp_val(DIV_ROUND_CLOSEST(fi->denominator, fi->numerator),
            minfps, maxfps);

    best_fps = minfps;
    for (i = 0; i < ARRAY_SIZE(ap1302_framerates); i++) {
        int curr_fps = ap1302_framerates[i];

        if (abs(curr_fps - fps) < abs(best_fps - fps)) {
            best_fps = curr_fps;
            rate = i;
        }
    }

    fi->numerator = 1;
    fi->denominator = best_fps;

find_mode:
    mode = ap1302_find_mode(sensor, rate, width, height, false);
    return mode ? rate : -EINVAL;
}

static int ap1302_get_fmt(struct v4l2_subdev *sd,
              struct v4l2_subdev_state *sd_state,
              struct v4l2_subdev_format *format)
{
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    struct v4l2_mbus_framefmt *fmt;

    if (format->pad != 0)
        return -EINVAL;

    mutex_lock(&sensor->lock);

    if (format->which == V4L2_SUBDEV_FORMAT_TRY)
        v4l2_subdev_get_try_format(&sensor->sd, sd_state,
                         format->pad);
    else
        fmt = &sensor->fmt;

    fmt->reserved[1] = (sensor->current_fr == AP1302_30_FPS) ? 30 : 15;
    format->format = *fmt;

    mutex_unlock(&sensor->lock);
    return 0;
}

static int ap1302_try_fmt_internal(struct v4l2_subdev *sd,
                   struct v4l2_mbus_framefmt *fmt,
                   enum ap1302_frame_rate fr,
                   const struct ap1302_mode_info **new_mode)
{
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    const struct ap1302_mode_info *mode;
    int i;

    mode = ap1302_find_mode(sensor, fr, fmt->width, fmt->height, true);
    if (!mode)
        return -EINVAL;
    fmt->width = mode->hact;
    fmt->height = mode->vact;
    memset(fmt->reserved, 0, sizeof(fmt->reserved));

    if (new_mode)
        *new_mode = mode;

    for (i = 0; i < ARRAY_SIZE(ap1302_formats); i++)
        if (ap1302_formats[i].code == fmt->code)
            break;
    if (i >= ARRAY_SIZE(ap1302_formats))
        i = 0;

    fmt->code = ap1302_formats[i].code;
    fmt->colorspace = ap1302_formats[i].colorspace;
    fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
    fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
    fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);

    return 0;
}

static int ap1302_set_fmt(struct v4l2_subdev *sd,
              struct v4l2_subdev_state *sd_state,
              struct v4l2_subdev_format *format)
{
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    const struct ap1302_mode_info *new_mode;
    struct v4l2_mbus_framefmt *mbus_fmt = &format->format;
    struct v4l2_mbus_framefmt *fmt;
    int ret;

    if (format->pad != 0)
        return -EINVAL;

    mutex_lock(&sensor->lock);

    if (sensor->streaming) {
        ret = -EBUSY;
        goto out;
    }

    ret = ap1302_try_fmt_internal(sd, mbus_fmt,
                      sensor->current_fr, &new_mode);
    if (ret)
        goto out;

    if (format->which == V4L2_SUBDEV_FORMAT_TRY)
        fmt = v4l2_subdev_get_try_format(sd, sd_state, 0);
    else
        fmt = &sensor->fmt;

    *fmt = *mbus_fmt;

    if (new_mode != sensor->current_mode) {
        sensor->current_mode = new_mode;
        sensor->pending_mode_change = true;
    }

    if (mbus_fmt->code != sensor->fmt.code)
        sensor->pending_fmt_change = true;

    __v4l2_ctrl_s_ctrl_int64(sensor->ctrls.pixel_rate,
                             ap1302_calc_pixel_rate(sensor));

    if (sensor->pending_mode_change || sensor->pending_fmt_change)
        sensor->fmt = *mbus_fmt;

out:
    mutex_unlock(&sensor->lock);
    return ret;
}

static int ap1302_set_framefmt(struct ap1302_dev *sensor,
                   struct v4l2_mbus_framefmt *format)
{
    int ret = 0;
    bool is_jpeg = false;
    u8 fmt, mux;

    switch (format->code) {
    case MEDIA_BUS_FMT_UYVY8_2X8:
    case MEDIA_BUS_FMT_UYVY8_1X16:
    case MEDIA_BUS_FMT_YUYV8_2X8:
    case MEDIA_BUS_FMT_YUYV8_1X16:
    ap1302_write(sensor, AP1302_PREVIEW_OUT_FMT,
                         AP1302_PREVIEW_OUT_FMT_FT_YUV_JFIF | AP1302_PREVIEW_OUT_FMT_FST_YUV_422, 
                         &ret);
        break;
      default:
        return -EINVAL;
        break;
    }

    return ret;
}

/*
 * Sensor Controls.
 */

static int ap1302_set_ctrl_hue(struct ap1302_dev *sensor, int value)
{
    return 0;
}

static int ap1302_set_ctrl_contrast(struct ap1302_dev *sensor, int value)
{
    return 0;
}

static int ap1302_set_ctrl_saturation(struct ap1302_dev *sensor, int value)
{
    return 0;
}

static int ap1302_set_ctrl_white_balance(struct ap1302_dev *sensor, int awb)
{
    return 0;
}

static int ap1302_set_ctrl_exposure(struct ap1302_dev *sensor,
                    enum v4l2_exposure_auto_type auto_exposure)
{
    return 0;
}

static int ap1302_set_ctrl_gain(struct ap1302_dev *sensor, bool auto_gain)
{
    return 0;
}

static const char * const test_pattern_menu[] = {
    "Disabled",
    "Color bars",
    "Color bars w/ rolling bar",
    "Color squares",
    "Color squares w/ rolling bar",
};

#define AP1302_TEST_ENABLE        BIT(7)
#define AP1302_TEST_ROLLING        BIT(6)    /* rolling horizontal bar */
#define AP1302_TEST_TRANSPARENT        BIT(5)
#define AP1302_TEST_SQUARE_BW        BIT(4)    /* black & white squares */
#define AP1302_TEST_BAR_STANDARD    (0 << 2)
#define AP1302_TEST_BAR_VERT_CHANGE_1    (1 << 2)
#define AP1302_TEST_BAR_HOR_CHANGE    (2 << 2)
#define AP1302_TEST_BAR_VERT_CHANGE_2    (3 << 2)
#define AP1302_TEST_BAR            (0 << 0)
#define AP1302_TEST_RANDOM        (1 << 0)
#define AP1302_TEST_SQUARE        (2 << 0)
#define AP1302_TEST_BLACK        (3 << 0)

static const u8 test_pattern_val[] = {
    0,
    AP1302_TEST_ENABLE | AP1302_TEST_BAR_VERT_CHANGE_1 |
        AP1302_TEST_BAR,
    AP1302_TEST_ENABLE | AP1302_TEST_ROLLING |
        AP1302_TEST_BAR_VERT_CHANGE_1 | AP1302_TEST_BAR,
    AP1302_TEST_ENABLE | AP1302_TEST_SQUARE,
    AP1302_TEST_ENABLE | AP1302_TEST_ROLLING | AP1302_TEST_SQUARE,
};

static int ap1302_set_ctrl_test_pattern(struct ap1302_dev *sensor, int value)
{
    return 0;
}

static int ap1302_set_ctrl_light_freq(struct ap1302_dev *sensor, int value)
{
    return 0;
}

static int ap1302_set_ctrl_hflip(struct ap1302_dev *sensor, int value)
{
    /*
     * If sensor is mounted upside down, mirror logic is inversed.
     *
     * Sensor is a BSI (Back Side Illuminated) one,
     * so image captured is physically mirrored.
     * This is why mirror logic is inversed in
     * order to cancel this mirror effect.
     */
    return 0;
}

static int ap1302_set_ctrl_vflip(struct ap1302_dev *sensor, int value)
{
    /* If sensor is mounted upside down, flip logic is inversed */
    return 0;
}

static int ap1302_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
    struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    int val;

    return 0;
}

static int ap1302_s_ctrl(struct v4l2_ctrl *ctrl)
{
    struct v4l2_subdev *sd = ctrl_to_sd(ctrl);
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    int ret;

    /* v4l2_ctrl_lock() locks our own mutex */

    /*
     * If the device is not powered up by the host driver do
     * not apply any controls to H/W at this time. Instead
     * the controls will be restored right after power-up.
     */
    if (sensor->power_count == 0)
        return 0;

    switch (ctrl->id) {
    case V4L2_CID_AUTOGAIN:
        ret = ap1302_set_ctrl_gain(sensor, ctrl->val);
        break;
    case V4L2_CID_EXPOSURE_AUTO:
        ret = ap1302_set_ctrl_exposure(sensor, ctrl->val);
        break;
    case V4L2_CID_AUTO_WHITE_BALANCE:
        ret = ap1302_set_ctrl_white_balance(sensor, ctrl->val);
        break;
    case V4L2_CID_HUE:
        ret = ap1302_set_ctrl_hue(sensor, ctrl->val);
        break;
    case V4L2_CID_CONTRAST:
        ret = ap1302_set_ctrl_contrast(sensor, ctrl->val);
        break;
    case V4L2_CID_SATURATION:
        ret = ap1302_set_ctrl_saturation(sensor, ctrl->val);
        break;
    case V4L2_CID_TEST_PATTERN:
        ret = ap1302_set_ctrl_test_pattern(sensor, ctrl->val);
        break;
    case V4L2_CID_POWER_LINE_FREQUENCY:
        ret = ap1302_set_ctrl_light_freq(sensor, ctrl->val);
        break;
    case V4L2_CID_HFLIP:
        ret = ap1302_set_ctrl_hflip(sensor, ctrl->val);
        break;
    case V4L2_CID_VFLIP:
        ret = ap1302_set_ctrl_vflip(sensor, ctrl->val);
        break;
    default:
        ret = -EINVAL;
        break;
    }

    return ret;
}

static const struct v4l2_ctrl_ops ap1302_ctrl_ops = {
    .g_volatile_ctrl = ap1302_g_volatile_ctrl,
    .s_ctrl = ap1302_s_ctrl,
};

static int ap1302_init_controls(struct ap1302_dev *sensor)
{
    const struct v4l2_ctrl_ops *ops = &ap1302_ctrl_ops;
    struct ap1302_ctrls *ctrls = &sensor->ctrls;
    struct v4l2_ctrl_handler *hdl = &ctrls->handler;
    int ret;

    v4l2_ctrl_handler_init(hdl, 32);

    /* we can use our own mutex for the ctrl lock */
    hdl->lock = &sensor->lock;

    /* Clock related controls */
    ctrls->pixel_rate = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_PIXEL_RATE,
                                          0, INT_MAX, 1,
                                          ap1302_calc_pixel_rate(sensor));

    /* Auto/manual white balance */
    ctrls->auto_wb = v4l2_ctrl_new_std(hdl, ops,
                       V4L2_CID_AUTO_WHITE_BALANCE,
                       0, 1, 1, 1);
    ctrls->blue_balance = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_BLUE_BALANCE,
                        0, 4095, 1, 0);
    ctrls->red_balance = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_RED_BALANCE,
                           0, 4095, 1, 0);
    /* Auto/manual exposure */
    ctrls->auto_exp = v4l2_ctrl_new_std_menu(hdl, ops,
                         V4L2_CID_EXPOSURE_AUTO,
                         V4L2_EXPOSURE_MANUAL, 0,
                         V4L2_EXPOSURE_AUTO);
    ctrls->exposure = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_EXPOSURE,
                        0, 65535, 1, 0);
    /* Auto/manual gain */
    ctrls->auto_gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_AUTOGAIN,
                         0, 1, 1, 1);
    ctrls->gain = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_GAIN,
                    0, 1023, 1, 0);

    ctrls->saturation = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_SATURATION,
                          0, 255, 1, 64);
    ctrls->hue = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HUE,
                       0, 359, 1, 0);
    ctrls->contrast = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_CONTRAST,
                        0, 255, 1, 0);
    ctrls->test_pattern =
        v4l2_ctrl_new_std_menu_items(hdl, ops, V4L2_CID_TEST_PATTERN,
                         ARRAY_SIZE(test_pattern_menu) - 1,
                         0, 0, test_pattern_menu);
    ctrls->hflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_HFLIP,
                     0, 1, 1, 0);
    ctrls->vflip = v4l2_ctrl_new_std(hdl, ops, V4L2_CID_VFLIP,
                     0, 1, 1, 0);

    ctrls->light_freq =
        v4l2_ctrl_new_std_menu(hdl, ops,
                       V4L2_CID_POWER_LINE_FREQUENCY,
                       V4L2_CID_POWER_LINE_FREQUENCY_AUTO, 0,
                       V4L2_CID_POWER_LINE_FREQUENCY_50HZ);

    if (hdl->error) {
        ret = hdl->error;
        goto free_ctrls;
    }

    ctrls->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;
    ctrls->gain->flags |= V4L2_CTRL_FLAG_VOLATILE;
    ctrls->exposure->flags |= V4L2_CTRL_FLAG_VOLATILE;

    v4l2_ctrl_auto_cluster(3, &ctrls->auto_wb, 0, false);
    v4l2_ctrl_auto_cluster(2, &ctrls->auto_gain, 0, true);
    v4l2_ctrl_auto_cluster(2, &ctrls->auto_exp, 1, true);

    sensor->sd.ctrl_handler = hdl;
    return 0;

free_ctrls:
    v4l2_ctrl_handler_free(hdl);
    return ret;
}

static int ap1302_enum_frame_size(struct v4l2_subdev *sd,
                  struct v4l2_subdev_state *sd_state,
                  struct v4l2_subdev_frame_size_enum *fse)
{
    if (fse->pad != 0)
        return -EINVAL;
    if (fse->index >= AP1302_NUM_MODES)
        return -EINVAL;

    fse->min_width =
        ap1302_mode_data[fse->index].hact;
    fse->max_width = fse->min_width;
    fse->min_height =
        ap1302_mode_data[fse->index].vact;
    fse->max_height = fse->min_height;

    return 0;
}

static int ap1302_enum_frame_interval(
    struct v4l2_subdev *sd,
    struct v4l2_subdev_state *sd_state,
    struct v4l2_subdev_frame_interval_enum *fie)
{
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    int i, j, count;

    if (fie->pad != 0)
        return -EINVAL;
    if (fie->index >= AP1302_NUM_FRAMERATES)
        return -EINVAL;

    if (fie->width == 0 || fie->height == 0 || fie->code == 0) {
        pr_warn("Please assign pixel format, width and height.\n");
        return -EINVAL;
    }

    fie->interval.numerator = 1;

    count = 0;
    for (i = 0; i < AP1302_NUM_FRAMERATES; i++) {
        for (j = 0; j < AP1302_NUM_MODES; j++) {
            if (fie->width  == ap1302_mode_data[j].hact &&
                fie->height == ap1302_mode_data[j].vact &&
                !ap1302_check_valid_mode(sensor, &ap1302_mode_data[j], i))
                count++;

            if (fie->index == (count - 1)) {
                fie->interval.denominator = ap1302_framerates[i];
                return 0;
            }
        }
    }

    return -EINVAL;
}

static int ap1302_g_frame_interval(struct v4l2_subdev *sd,
                   struct v4l2_subdev_frame_interval *fi)
{
    struct ap1302_dev *sensor = to_ap1302_dev(sd);

    mutex_lock(&sensor->lock);
    fi->interval = sensor->frame_interval;
    mutex_unlock(&sensor->lock);

    return 0;
}

static int ap1302_s_frame_interval(struct v4l2_subdev *sd,
                   struct v4l2_subdev_frame_interval *fi)
{
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    const struct ap1302_mode_info *mode;
    int frame_rate, ret = 0;

    if (fi->pad != 0)
        return -EINVAL;

    mutex_lock(&sensor->lock);

    if (sensor->streaming) {
        ret = -EBUSY;
        goto out;
    }

    mode = sensor->current_mode;

    frame_rate = ap1302_try_frame_interval(sensor, &fi->interval,
                           mode->hact, mode->vact);
    if (frame_rate < 0) {
        /* Always return a valid frame interval value */
        fi->interval = sensor->frame_interval;
        goto out;
    }

    mode = ap1302_find_mode(sensor, frame_rate, mode->hact,
                mode->vact, true);
    if (!mode) {
        ret = -EINVAL;
        goto out;
    }

    if (mode != sensor->current_mode ||
        frame_rate != sensor->current_fr) {
        sensor->current_fr = frame_rate;
        sensor->frame_interval = fi->interval;
        sensor->current_mode = mode;
        sensor->pending_mode_change = true;
        __v4l2_ctrl_s_ctrl_int64(sensor->ctrls.pixel_rate,
                                 ap1302_calc_pixel_rate(sensor));
    }
out:
    mutex_unlock(&sensor->lock);
    return ret;
}

static int ap1302_enum_mbus_code(struct v4l2_subdev *sd,
                 struct v4l2_subdev_state *sd_state,
                 struct v4l2_subdev_mbus_code_enum *code)
{
    struct ap1302_dev *sensor = to_ap1302_dev(sd);

    if (code->pad != 0)
        return -EINVAL;
    if (code->index >= ARRAY_SIZE(ap1302_formats))
        return -EINVAL;

    code->code = ap1302_formats[code->index].code;
    return 0;
}

static int ap1302_s_stream(struct v4l2_subdev *sd, int enable)
{
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    int ret = 0;

    mutex_lock(&sensor->lock);

    if (sensor->streaming == !enable) {
        ret = ap1302_check_valid_mode(sensor,
                          sensor->current_mode,
                          sensor->current_fr);
        if (ret) {
            dev_err(sensor->dev, "Not support WxH@fps=%dx%d@%d\n",
                sensor->current_mode->hact,
                sensor->current_mode->vact,
                ap1302_framerates[sensor->current_fr]);
            goto out;
        }

        if (enable && sensor->pending_mode_change) {
            ret = ap1302_set_mode(sensor);
            if (ret)
                goto out;
        }

        if (enable && sensor->pending_fmt_change) {
            ret = ap1302_set_framefmt(sensor, &sensor->fmt);
            if (ret)
                goto out;
            sensor->pending_fmt_change = false;
        }

        if (sensor->ep.bus_type == V4L2_MBUS_CSI2_DPHY)
            ret = ap1302_set_stream_mipi(sensor, enable);
        else
            ret = ap1302_set_stream_dvp(sensor, enable);

        if (!ret)
            sensor->streaming = enable;
    }
out:
    mutex_unlock(&sensor->lock);
    return ret;
}

static const struct v4l2_subdev_core_ops ap1302_core_ops = {
    .s_power = ap1302_s_power,
    .log_status = v4l2_ctrl_subdev_log_status,
    .subscribe_event = v4l2_ctrl_subdev_subscribe_event,
    .unsubscribe_event = v4l2_event_subdev_unsubscribe,
};

static const struct v4l2_subdev_video_ops ap1302_video_ops = {
    .g_frame_interval = ap1302_g_frame_interval,
    .s_frame_interval = ap1302_s_frame_interval,
    .s_stream = ap1302_s_stream,
};

static const struct v4l2_subdev_pad_ops ap1302_pad_ops = {
    .enum_mbus_code = ap1302_enum_mbus_code,
    .get_fmt = ap1302_get_fmt,
    .set_fmt = ap1302_set_fmt,
    .enum_frame_size = ap1302_enum_frame_size,
    .enum_frame_interval = ap1302_enum_frame_interval,
};

static const struct v4l2_subdev_ops ap1302_subdev_ops = {
    .core = &ap1302_core_ops,
    .video = &ap1302_video_ops,
    .pad = &ap1302_pad_ops,
};

static int ap1302_link_setup(struct media_entity *entity,
               const struct media_pad *local,
               const struct media_pad *remote, u32 flags)
{
    return 0;
}

static const struct media_entity_operations ap1302_sd_media_ops = {
    .link_setup = ap1302_link_setup,
};

static int ap1302_get_regulators(struct ap1302_dev *sensor)
{
    int i;

    for (i = 0; i < AP1302_NUM_SUPPLIES; i++)
        sensor->supplies[i].supply = ap1302_supply_name[i];

    return devm_regulator_bulk_get(sensor->dev,
                       AP1302_NUM_SUPPLIES,
                       sensor->supplies);
}


static int ap1302_detect_chip(struct ap1302_dev *sensor)
{
    unsigned int version;
    unsigned int revision;
    int ret;

    ret = ap1302_read(sensor, AP1302_CHIP_VERSION, &version);
    if (ret)
        return ret;

    ret = ap1302_read(sensor, AP1302_CHIP_REV, &revision);
    if (ret)
        return ret;

    if (version != AP1302_CHIP_ID) {
        dev_err(sensor->dev,
                "Invalid chip version, expected 0x%04x, got 0x%04x\n",
                AP1302_CHIP_ID, version);
        return -EINVAL;
    }

    dev_info(sensor->dev, "AP1302 revision %u.%u.%u detected\n",
            (revision & 0xf000) >> 12, (revision & 0x0f00) >> 8,
             revision & 0x00ff);

    return 0;
}



static int ap1302_stall(struct ap1302_dev *sensor, bool stall)
{
    int ret = 0;

    if (stall) {
        ap1302_write(sensor, AP1302_SYS_START,
                     AP1302_SYS_START_PLL_LOCK |
                     AP1302_SYS_START_STALL_MODE_DISABLED, &ret);
        ap1302_write(sensor, AP1302_SYS_START,
                     AP1302_SYS_START_PLL_LOCK |
                     AP1302_SYS_START_STALL_EN |
                     AP1302_SYS_START_STALL_MODE_DISABLED, &ret);
        if (ret < 0)
            return ret;

        msleep(200);

        ap1302_write(sensor, AP1302_ADV_IRQ_SYS_INTE,
                     AP1302_ADV_IRQ_SYS_INTE_SIPM |
                     AP1302_ADV_IRQ_SYS_INTE_SIPS_FIFO_WRITE, &ret);
        if (ret < 0)
            return ret;

        sensor->streaming = false;
        return 0;
    } else {
        sensor->streaming = true;
        return ap1302_write(sensor, AP1302_SYS_START,
                            AP1302_SYS_START_PLL_LOCK |
                            AP1302_SYS_START_STALL_STATUS |
                            AP1302_SYS_START_STALL_EN |
                            AP1302_SYS_START_STALL_MODE_DISABLED, NULL);
    }
}

static int ap1302_set_mipi_t3_clk(struct ap1302_dev *sensor)
{
    unsigned int mipi_t3, t_clk_post, t_clk_pre;
    int ret;

    /* Set the Tclk_post and Tclk_pre values */
    ret = ap1302_read(sensor, AP1302_ADV_HINF_MIPI_T3, &mipi_t3);
    if (ret)
        return ret;

    /* Read Tclk post default setting and increment by 2 */
    t_clk_post = ((mipi_t3 & AP1302_TCLK_POST_MASK)
                    >> AP1302_TCLK_POST_SHIFT) + 0x5;
    /* Read Tclk pre default setting and increment by 1 */
    t_clk_pre = ((mipi_t3 & AP1302_TCLK_PRE_MASK)
                    >> AP1302_TCLK_PRE_SHIFT) + 0x1;

    mipi_t3 = ((mipi_t3 & ~(AP1302_TCLK_POST_MASK))
                    & ~(AP1302_TCLK_PRE_MASK));
    mipi_t3 = (mipi_t3 | (t_clk_pre << AP1302_TCLK_PRE_SHIFT)
                    | t_clk_post);

    /* Write MIPI_T3 register with updated Tclk_post and Tclk_pre values */
    return ap1302_write(sensor, AP1302_ADV_HINF_MIPI_T3, mipi_t3, NULL);
}


/* -----------------------------------------------------------------------------
 * Boot & Firmware Handling
 */

static int ap1302_request_firmware(struct ap1302_dev *sensor)
{
    static const char * const suffixes[] = {
            "",
            "_single",
            "_dual",
    };

    const struct ap1302_firmware_header *fw_hdr;
    unsigned int num_sensors=1;
    unsigned int fw_size;
    unsigned int i;
    char name[64];
    int ret;


    ret = snprintf(name, sizeof(name), "ap1302_%s%s_fw.bin",
                   "ar0821", suffixes[num_sensors]);
    if (ret >= sizeof(name)) {
        dev_err(sensor->dev, "Firmware name too long\n");
        return -EINVAL;
    }

    dev_dbg(sensor->dev, "Requesting firmware %s\n", name);

    ret = request_firmware(&sensor->fw, name, sensor->dev);
    if (ret) {
        dev_err(sensor->dev, "Failed to request firmware: %d\n", ret);
        return ret;
    }

    /*
     * The firmware binary contains a header defined by the
     * ap1302_firmware_header structure. The firmware itself (also referred
     * to as bootdata) follows the header. Perform sanity checks to ensure
     * the firmware is valid.
     */
    fw_hdr = (const struct ap1302_firmware_header *)sensor->fw->data;
    fw_size = sensor->fw->size - sizeof(struct ap1302_firmware_header);

    if (fw_hdr->pll_init_size > fw_size) {
        dev_err(sensor->dev, "Invalid firmware: PLL init size too large\n");
        return -EINVAL;
    }

    return 0;
}

/*
 * ap1302_write_fw_window() - Write a piece of firmware to the AP1302
 * @win_pos: Firmware load window current position
 * @buf: Firmware data buffer
 * @len: Firmware data length
 *
 * The firmware is loaded through a window in the registers space. Writes are
 * sequential starting at address 0x8000, and must wrap around when reaching
 * 0x9fff. This function write the firmware data stored in @buf to the AP1302,
 * keeping track of the window position in the @win_pos argument.
 */
 
static int ap1302_write_fw_block(struct ap1302_dev *sensor, const u8 *buf,
                  u32 len, unsigned int *win_pos) {
    u32 block_num = len / AP1302_FW_BLOCK_LEN;
    u32 last_block_size = len % AP1302_FW_BLOCK_LEN;
    int i;
    int ret;
    unsigned int wr_pos = *win_pos;
    unsigned int write_addr;

    for(i=0; i<block_num; i++) {
        write_addr = wr_pos%AP1302_FW_WINDOW_SIZE + AP1302_FW_WINDOW_OFFSET;
        ret = regmap_raw_write(sensor->regmap16, write_addr, buf, AP1302_FW_BLOCK_LEN);
        if (ret) {
            dev_err(sensor->dev, "%s: regmap_raw_write error = %d\n", __func__, ret);
            return ret;
        }
        buf += AP1302_FW_BLOCK_LEN;
        wr_pos += AP1302_FW_BLOCK_LEN;
    }
    write_addr = wr_pos%AP1302_FW_WINDOW_SIZE + AP1302_FW_WINDOW_OFFSET;
    if(last_block_size > 0) {
        ret = regmap_raw_write(sensor->regmap16, write_addr, buf, last_block_size);
        if (ret) {
            dev_err(sensor->dev, "%s: regmap_raw_write error = %d\n", __func__, ret);
            return ret;
        }
        wr_pos += last_block_size;
    }

    *win_pos = wr_pos;
    return 0;
}

static int ap1302_write_fw_window(struct ap1302_dev *sensor, const u8 *buf,
                  u32 len, unsigned int *win_pos)
{
    u32 block_num = len / AP1302_FW_WINDOW_SIZE;
    u32 last_block_size = len % AP1302_FW_WINDOW_SIZE; 
    int i;
    int ret;
    
    for(i=0;i<block_num;i++) {
        ret = ap1302_write_fw_block(sensor, buf, AP1302_FW_WINDOW_SIZE, win_pos);
        if(ret) {
            dev_err(sensor->dev, "%s: error = %d\n", __func__, ret);
            return ret;
        }
        buf += AP1302_FW_WINDOW_SIZE;
    }
    if(last_block_size>0) {
        ret = ap1302_write_fw_block(sensor, buf, last_block_size, win_pos);
        if (ret) {
            dev_err(sensor->dev, "%s: error = %d\n", __func__, ret);
            return ret;
        }
    }
 
    return ret;
}

static int ap1302_load_firmware(struct ap1302_dev *sensor)
{
    const struct ap1302_firmware_header *fw_hdr;
    unsigned int fw_size;
    const u8 *fw_data;
    unsigned int win_pos = 0;
    unsigned int crc;
    int ret;

    fw_hdr = (const struct ap1302_firmware_header *)sensor->fw->data;
    fw_data = (u8 *)&fw_hdr[1];
    fw_size = sensor->fw->size - sizeof(struct ap1302_firmware_header);

    ret = ap1302_write_fw_window(sensor, fw_data, fw_size,
                     &win_pos);
    if (ret)
        return ret;

    /*
     * Write 0xffff to the bootdata_stage register to indicate to the
     * AP1302 that the whole bootdata content has been loaded.
     */    
    ret = ap1302_write(sensor, AP1302_BOOTDATA_STAGE, 0xffff, NULL);
    usleep_range(40*1000, 80*1000);
    ret |= ap1302_read(sensor, AP1302_SIP_CHECKSUM, &crc);
    if (ret)
        return ret;

    if (crc != 0xffff) {
        dev_warn(sensor->dev,
             "CRC mismatch: expected 0x%04x, got 0x%04x\n",
             0xffff, crc);
        return -EAGAIN;
    }

    /* Adjust MIPI TCLK timings */
    return ap1302_set_mipi_t3_clk(sensor);
}


static int ap1302_hw_init(struct ap1302_dev *sensor)
{
    unsigned int retries;
    int ret;

    /* Request and validate the firmware. */
    ret = ap1302_request_firmware(sensor);
    if (ret)
        return ret;

    /*
     * Power the sensors first, as the firmware will access them once it
     * gets loaded.
     */
    ret = ap1302_set_power_on(sensor);
    if (ret < 0)
        goto error_firmware;

    /*
     * Load the firmware, retrying in case of CRC errors. The AP1302 is
     * reset with a full power cycle between each attempt.
     */
    for (retries = 0; retries < MAX_FW_LOAD_RETRIES; ++retries) {
        ret = ap1302_power_on(sensor);
        if (ret < 0)
            goto error_power;

        ret = ap1302_detect_chip(sensor);
        if (ret)
            goto error_power;

        ret = ap1302_load_firmware(sensor);
        if (!ret)
            break;

        if (ret != -EAGAIN)
            goto error_power;

        ap1302_power_off(sensor);
    }

    if (retries == MAX_FW_LOAD_RETRIES) {
        dev_err(sensor->dev, "Firmware load retries exceeded, aborting\n");
        ret = -ETIMEDOUT;
        goto error_power;
    }

    return 0;

error_power:
    ap1302_set_power_off(sensor);
error_firmware:
    release_firmware(sensor->fw);

    return ret;
}

static void ap1302_hw_cleanup(struct ap1302_dev *sensor)
{
    ap1302_set_power_off(sensor);
}

static int ap1302_probe(struct i2c_client *client)
{
    struct device *dev = &client->dev;
    struct fwnode_handle *endpoint;
    struct ap1302_dev *sensor;
    struct v4l2_mbus_framefmt *fmt;
    u32 rotation;
    int ret;

    sensor = devm_kzalloc(dev, sizeof(*sensor), GFP_KERNEL);
    if (!sensor)
        return -ENOMEM;

    sensor->i2c_client = client;
    sensor->dev = dev;

    sensor->regmap16 = devm_regmap_init_i2c(client, &ap1302_reg16_config);
    if (IS_ERR(sensor->regmap16)) {
        dev_err(dev, "regmap16 init failed: %ld\n",
        PTR_ERR(sensor->regmap16));
        ret = -ENODEV;
        return ret;
    }

    sensor->regmap32 = devm_regmap_init_i2c(client, &ap1302_reg32_config);
    if (IS_ERR(sensor->regmap32)) {
        dev_err(dev, "regmap32 init failed: %ld\n",
        PTR_ERR(sensor->regmap32));
        ret = -ENODEV;
        return ret;
    }

    /*
     * default init sequence initialize sensor to
     * YUV422 UYVY VGA@30fps
     */
    fmt = &sensor->fmt;
    fmt->code = MEDIA_BUS_FMT_UYVY8_1X16;
    fmt->colorspace = V4L2_COLORSPACE_SRGB;
    fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
    fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
    fmt->xfer_func = V4L2_MAP_XFER_FUNC_DEFAULT(fmt->colorspace);
    fmt->width = 3840;
    fmt->height = 2160;
    fmt->field = V4L2_FIELD_NONE;
    sensor->frame_interval.numerator = 1;
    sensor->frame_interval.denominator = ap1302_framerates[AP1302_30_FPS];
    sensor->current_fr = AP1302_30_FPS;
    sensor->current_mode =
        &ap1302_mode_data[AP1302_MODE_4K_3840_2160];
    sensor->last_mode = sensor->current_mode;

    sensor->ae_target = 52;

    /* optional indication of physical rotation of sensor */
    ret = fwnode_property_read_u32(dev_fwnode(sensor->dev), "rotation",
                       &rotation);
    if (!ret) {
        switch (rotation) {
        case 180:
            sensor->upside_down = true;
            /* fall through */
        case 0:
            break;
        default:
            dev_warn(dev, "%u degrees rotation is not supported, ignoring...\n",
                 rotation);
        }
    }

    endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(sensor->dev),
                          NULL);
    if (!endpoint) {
        dev_err(dev, "endpoint node not found\n");
        return -EINVAL;
    }

    ret = v4l2_fwnode_endpoint_parse(endpoint, &sensor->ep);
    fwnode_handle_put(endpoint);
    if (ret) {
        dev_err(dev, "Could not parse endpoint\n");
        return ret;
    }

    if (sensor->ep.bus_type != V4L2_MBUS_PARALLEL &&
        sensor->ep.bus_type != V4L2_MBUS_CSI2_DPHY &&
        sensor->ep.bus_type != V4L2_MBUS_BT656) {
        dev_err(dev, "Unsupported bus type %d\n", sensor->ep.bus_type);
        return -EINVAL;
    }

    /* get system clock (xclk) */
    sensor->xclk = devm_clk_get(dev, "xclk");
    if (IS_ERR(sensor->xclk)) {
        dev_err(dev, "failed to get xclk\n");
        return PTR_ERR(sensor->xclk);
    }

    sensor->xclk_freq = clk_get_rate(sensor->xclk);
    if (sensor->xclk_freq < AP1302_XCLK_MIN ||
        sensor->xclk_freq > AP1302_XCLK_MAX) {
        dev_err(dev, "xclk frequency out of range: %d Hz\n",
            sensor->xclk_freq);
        return -EINVAL;
    }

    /* request optional power down pin */
    sensor->pwdn_gpio = devm_gpiod_get_optional(dev, "powerdown",
                            GPIOD_OUT_HIGH);
    if (IS_ERR(sensor->pwdn_gpio))
        return PTR_ERR(sensor->pwdn_gpio);

    /* request optional reset pin */
    sensor->reset_gpio = devm_gpiod_get_optional(dev, "reset",
                             GPIOD_OUT_HIGH);
    if (IS_ERR(sensor->reset_gpio))
        return PTR_ERR(sensor->reset_gpio);

    v4l2_i2c_subdev_init(&sensor->sd, client, &ap1302_subdev_ops);

    sensor->sd.flags |= V4L2_SUBDEV_FL_HAS_EVENTS;
    sensor->pad.flags = MEDIA_PAD_FL_SOURCE;
    sensor->sd.entity.ops = &ap1302_sd_media_ops;
    sensor->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
    ret = media_entity_pads_init(&sensor->sd.entity, 1, &sensor->pad);
    if (ret)
        return ret;

    ret = ap1302_get_regulators(sensor);
    if (ret)
        return ret;

    mutex_init(&sensor->lock);

    ret = ap1302_init_controls(sensor);
    if (ret)
        goto entity_cleanup;

    ret = v4l2_async_register_subdev_sensor(&sensor->sd);
    if (ret)
        goto free_ctrls;

    ret = ap1302_hw_init(sensor);
    if (ret)
        goto unreg_dev;
        
    dev_info(dev, "ap1302 ISP is found\n");
    return 0;

unreg_dev:
    v4l2_async_unregister_subdev(&sensor->sd);
free_ctrls:
    v4l2_ctrl_handler_free(&sensor->ctrls.handler);
entity_cleanup:
    mutex_destroy(&sensor->lock);
    media_entity_cleanup(&sensor->sd.entity);
    return ret;
}

static int ap1302_remove(struct i2c_client *client)
{
    struct v4l2_subdev *sd = i2c_get_clientdata(client);
    struct ap1302_dev *sensor = to_ap1302_dev(sd);
    ap1302_hw_cleanup(sensor);
    v4l2_async_unregister_subdev(&sensor->sd);
    media_entity_cleanup(&sensor->sd.entity);
    v4l2_ctrl_handler_free(&sensor->ctrls.handler);
    mutex_destroy(&sensor->lock);

    return 0;
}

static const struct i2c_device_id ap1302_id[] = {
    {"ap1302", 0},
    {},
};
MODULE_DEVICE_TABLE(i2c, ap1302_id);

static const struct of_device_id ap1302_dt_ids[] = {
    { .compatible = "onnn,ap1302" },
    { /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, ap1302_dt_ids);

static struct i2c_driver ap1302_i2c_driver = {
    .driver = {
        .name  = "ap1302",
        .of_match_table    = ap1302_dt_ids,
    },
    .id_table = ap1302_id,
    .probe_new = ap1302_probe,
    .remove   = ap1302_remove,
};

module_i2c_driver(ap1302_i2c_driver);

MODULE_DESCRIPTION("AP1302 MIPI Camera Subdev Driver");
MODULE_LICENSE("GPL");
