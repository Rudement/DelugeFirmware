/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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
#ifndef DELUGE_CV_AUDIO_STREAM_C_INTERFACE_H
#define DELUGE_CV_AUDIO_STREAM_C_INTERFACE_H

#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// The two halves of the shared-SPI handover, reachable from C.
///
/// The arbitration has to live in RZA1/oled/oled_low_level.c, because that file is the only
/// thing that knows when the bus is genuinely idle -- the display's chip-select is driven by
/// the PIC over UART, so "the transfer finished" is an event that arrives there and nowhere
/// else. That file is C, and the stream is C++ inside a namespace, hence this.
///
/// Both are safe to call from interrupt context, both are idempotent, and both do nothing
/// at all while the stream is not running. See cv_audio_stream.h for what they cost.
void cvStreamYieldBegin(void);
void cvStreamYieldEnd(void);

/// True while the send stream owns the shared bus -- running, and not handed over to the
/// display.
///
/// cvSPITransferComplete() needs this. That handler is registered on the RSPI receive
/// interrupt and exists to finish one hand-driven CV word: it deselects the converter, resets
/// the receive buffer, clears spiBusCurrentlySending and hands the bus back. Every one of
/// those is wrong when the word that completed was the stream's own, and clearing the
/// display's ownership flag out from under it is enough to stop the screen being redrawn and
/// leave the machine sitting on a frozen frame.
///
/// It never mattered while the stream's transmit sequence never ended, because then the
/// interrupt never fired during streaming. Per-word chip-select framing makes sequences end
/// tens of thousands of times a second, so it matters now.
bool cvStreamHoldsBus(void);

#ifdef __cplusplus
}
#endif

#endif // DELUGE_CV_AUDIO_STREAM_C_INTERFACE_H
