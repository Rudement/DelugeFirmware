/*
 * Copyright © 2019-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdint.h>

/* Direction names are the endpoint's, which are host-relative:
 *   ISO_IN  - audio leaving the Deluge for the host.
 *   ISO_OUT - audio arriving from the host.
 *
 * Only PIPE1 and PIPE2 can carry isochronous transfers (usb_pstd_chk_pipe_info), and both are taken - which is
 * also why there is no feedback endpoint. USB MIDI's bulk IN moved to PIPE4 to free PIPE2. */
#define USB_CFG_PAUDIO_ISO_IN (USB_PIPE1)
#define USB_CFG_PAUDIO_ISO_OUT (USB_PIPE2)

/* Outgoing channels. The descriptors and the packet builder both derive from this. */
#define USB_CFG_PAUDIO_CHANNELS (8u)

/* The largest outgoing packet, in audio frames. A bandwidth reservation rather than a buffer size: the host
 * reserves against what the endpoint declares, and a Full Speed frame has room for this alongside the return.
 * Packets are normally 44 or 45 frames; the margin is for catching up after a stall. */
#define USB_CFG_PAUDIO_MAX_FRAMES (46u)

/* Holds the 736-byte largest packet. */
#define USB_CFG_PAUDIO_BUF_BYTES (1024u)

/* Pipe buffer blocks, with double buffering counted:
 *
 *   MIDI in   8-23    audio out  24-55    MIDI out  72-87    return  88-105
 *
 * Anything added here counts its own doubling against this map. */
#define USB_CFG_PAUDIO_BUF_START (24u)

/* Returning channels. Must be even: see usb_audio_stream.cpp. */
#define USB_CFG_PAUDIO_RX_CHANNELS (4u)

/* The largest returning packet: 45 frames, which is what a 44.1 kHz host sends in a 1 ms frame. */
#define USB_CFG_PAUDIO_RX_MAX_FRAMES (45u)
#define USB_CFG_PAUDIO_RX_PACKET_BYTES (USB_CFG_PAUDIO_RX_CHANNELS * 2u * USB_CFG_PAUDIO_RX_MAX_FRAMES)
/* Room for six channels, so the buffer map does not move if the return is widened. */
#define USB_CFG_PAUDIO_RX_BUF_BYTES (576u)
#define USB_CFG_PAUDIO_RX_BUF_START (88u)

#ifdef __cplusplus
extern "C" {
#endif
/* From the peripheral interrupt's frame branch, on the host's 1 kHz clock. */
void usbAudioStreamStartOfFrame(void);

/* Non-zero while the DMA controller, not the driver, is reading the return pipe. */
extern volatile uint8_t usbReturnPipeUnderDma;
#ifdef __cplusplus
}
#endif
