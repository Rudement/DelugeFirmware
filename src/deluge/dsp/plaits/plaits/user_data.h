// Deluge port stub for plaits/user_data.h
//
// The original header reads a user-supplied wavetable / FM-patch bank out of a
// reserved page of the STM32's internal flash, and so pulls in
// <stm32f37x_conf.h> and stmlib/system/flash_programming.h. Neither exists on
// the RZ/A1L, and the feature itself (loading custom data over the module's
// audio-modem bootloader) has no Deluge equivalent.
//
// This stub keeps plaits/dsp/voice.cc compiling byte-identical to upstream by
// reporting "no user data present", which is exactly what an unprogrammed
// Plaits reports. Voice::Render then leaves every engine on its factory data.
//
// If user data is ever wanted on the Deluge it should be read from the SD card
// and handed to Engine::LoadUserData() directly; do not resurrect this path.

#ifndef PLAITS_USER_DATA_H_
#define PLAITS_USER_DATA_H_

#include <cstdint>
#include <cstddef>

namespace plaits {

class UserData {
public:
	enum { SIZE = 0x1000 };

	UserData() {}
	~UserData() {}

	// Upstream returns a pointer into flash, or NULL when the slot is empty.
	// Always empty here.
	inline const uint8_t* ptr(int slot) const { return nullptr; }

	inline bool Save(const uint8_t* rx_buffer, int slot) { return false; }
};

} // namespace plaits

#endif // PLAITS_USER_DATA_H_
