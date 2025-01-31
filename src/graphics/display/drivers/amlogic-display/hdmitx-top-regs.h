// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_GRAPHICS_DISPLAY_DRIVERS_AMLOGIC_DISPLAY_HDMITX_TOP_REGS_H_
#define SRC_GRAPHICS_DISPLAY_DRIVERS_AMLOGIC_DISPLAY_HDMITX_TOP_REGS_H_

// TOP
#define TOP_OFFSET_MASK (0x0UL << 24)

#define HDMITX_TOP_SW_RESET (TOP_OFFSET_MASK + 0x000)
#define HDMITX_TOP_CLK_CNTL (TOP_OFFSET_MASK + 0x001)
#define HDMITX_TOP_HPD_FILTER (TOP_OFFSET_MASK + 0x002)
#define HDMITX_TOP_INTR_MASKN (TOP_OFFSET_MASK + 0x003)
#define HDMITX_TOP_INTR_STAT (TOP_OFFSET_MASK + 0x004)
#define HDMITX_TOP_INTR_STAT_CLR (TOP_OFFSET_MASK + 0x005)
#define HDMITX_TOP_BIST_CNTL (TOP_OFFSET_MASK + 0x006)
#define HDMITX_TOP_SHIFT_PTTN_012 (TOP_OFFSET_MASK + 0x007)
#define HDMITX_TOP_SHIFT_PTTN_345 (TOP_OFFSET_MASK + 0x008)
#define HDMITX_TOP_SHIFT_PTTN_67 (TOP_OFFSET_MASK + 0x009)
#define HDMITX_TOP_TMDS_CLK_PTTN_01 (TOP_OFFSET_MASK + 0x00A)
#define HDMITX_TOP_TMDS_CLK_PTTN_23 (TOP_OFFSET_MASK + 0x00B)
#define HDMITX_TOP_TMDS_CLK_PTTN_CNTL (TOP_OFFSET_MASK + 0x00C)
#define HDMITX_TOP_REVOCMEM_STAT (TOP_OFFSET_MASK + 0x00D)
#define HDMITX_TOP_STAT0 (TOP_OFFSET_MASK + 0x00E)
#define HDMITX_TOP_SKP_CNTL_STAT (TOP_SEC_OFFSET_MASK + 0x010)
#define HDMITX_TOP_NONCE_0 (TOP_SEC_OFFSET_MASK + 0x011)
#define HDMITX_TOP_NONCE_1 (TOP_SEC_OFFSET_MASK + 0x012)
#define HDMITX_TOP_NONCE_2 (TOP_SEC_OFFSET_MASK + 0x013)
#define HDMITX_TOP_NONCE_3 (TOP_SEC_OFFSET_MASK + 0x014)
#define HDMITX_TOP_PKF_0 (TOP_SEC_OFFSET_MASK + 0x015)
#define HDMITX_TOP_PKF_1 (TOP_SEC_OFFSET_MASK + 0x016)
#define HDMITX_TOP_PKF_2 (TOP_SEC_OFFSET_MASK + 0x017)
#define HDMITX_TOP_PKF_3 (TOP_SEC_OFFSET_MASK + 0x018)
#define HDMITX_TOP_DUK_0 (TOP_SEC_OFFSET_MASK + 0x019)
#define HDMITX_TOP_DUK_1 (TOP_SEC_OFFSET_MASK + 0x01A)
#define HDMITX_TOP_DUK_2 (TOP_SEC_OFFSET_MASK + 0x01B)
#define HDMITX_TOP_DUK_3 (TOP_SEC_OFFSET_MASK + 0x01C)
#define HDMITX_TOP_INFILTER (TOP_OFFSET_MASK + 0x01D)
#define HDMITX_TOP_NSEC_SCRATCH (TOP_OFFSET_MASK + 0x01E)
#define HDMITX_TOP_SEC_SCRATCH (TOP_SEC_OFFSET_MASK + 0x01F)
#define HDMITX_TOP_DONT_TOUCH0 (TOP_OFFSET_MASK + 0x0FE)
#define HDMITX_TOP_DONT_TOUCH1 (TOP_OFFSET_MASK + 0x0FF)

#endif  // SRC_GRAPHICS_DISPLAY_DRIVERS_AMLOGIC_DISPLAY_HDMITX_TOP_REGS_H_
