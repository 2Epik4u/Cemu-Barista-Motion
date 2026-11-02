#pragma once
#include "input/api/ControllerProvider.h"
#include "input/api/ControllerState.h"
#include "input/motion/MotionHandler.h"
#include <mutex>

// Input-only adapter to the local daemon. No pairing, credentials or UDP ports.
class WiiUGamePadControllerProvider : public ControllerProviderBase
{
public:
	inline static InputAPI::Type kAPIType = InputAPI::WiiUGamePad;
	InputAPI::Type api() const override { return kAPIType; }
	std::vector<std::shared_ptr<ControllerBase>> get_controllers() override;
	bool is_connected(size_t index) const;
	ControllerState get_state(size_t index) const;
	MotionSample get_motion_sample(size_t index);

private:
	std::mutex m_motion_mutex;
	WiiUMotionHandler m_motion_handler;
	MotionSample m_motion_sample;
	uint64 m_connection_revision = 0;
	uint16 m_last_motion_sequence = 0;
	bool m_motion_sequence_valid = false;
};
