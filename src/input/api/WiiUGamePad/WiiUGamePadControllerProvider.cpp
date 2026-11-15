#include "input/api/WiiUGamePad/WiiUGamePadControllerProvider.h"
#include "input/api/WiiUGamePad/WiiUGamePadController.h"
#include "Common/BaristaAppHook.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string_view>

namespace
{
constexpr std::array<uint32, 18> s_buttonMasks{0x8000, 0x4000, 0x2000, 0x1000,
	0x20, 0x10, 0x80, 0x40, 0x4, 0x8, 0x2, 0x200, 0x100, 0x800, 0x400,
	0x800000, 0x400000, 0x200000};

constexpr uint32 DecodeButtons(const std::array<uint8, 128>& report)
{
	// Unlike the little-endian stick fields, the main button mask is big-endian.
	return uint32(report[80]) << 16 | uint32(report[2]) << 8 | report[3];
}

constexpr bool ValidateButtonDecoder()
{
	// Wire bytes -> Cemu physical button index. Independent protocol fixtures
	// catch byte swapping (e.g. A becoming ZL) and the X/Y mask reversal.
	constexpr std::array<std::array<uint8, 4>, 18> cases{{
		{0x80,0,0,0}, {0x40,0,0,1}, {0x20,0,0,2}, {0x10,0,0,3},
		{0,0x20,0,4}, {0,0x10,0,5}, {0,0x80,0,6}, {0,0x40,0,7},
		{0,4,0,8}, {0,8,0,9}, {0,2,0,10}, {2,0,0,11},
		{1,0,0,12}, {8,0,0,13}, {4,0,0,14},
		{0,0,0x80,15}, {0,0,0x40,16}, {0,0,0x20,17}}};
	for (const auto& test : cases)
	{
		std::array<uint8, 128> report{};
		report[2] = test[0]; report[3] = test[1]; report[80] = test[2];
		for (size_t i = 0; i < s_buttonMasks.size(); ++i)
			if (((DecodeButtons(report) & s_buttonMasks[i]) != 0) != (i == test[3]))
				return false;
	}
	return true;
}
static_assert(ValidateButtonDecoder());

struct MotionReading
{
	std::array<float, 3> acceleration{};
	std::array<float, 3> gyroscope{};
};

constexpr sint32 DecodeSigned24(const uint8* data)
{
	const uint32 value = uint32(data[0]) | uint32(data[1]) << 8 | uint32(data[2]) << 16;
	return (value & 0x00800000u) != 0 ? static_cast<sint32>(value) - 0x01000000 : static_cast<sint32>(value);
}

constexpr MotionReading DecodeMotion(const std::array<uint8, 128>& report)
{
	// Physical sensor mapping derived from a real Wii U GamePad gravity/gyro
	// capture. Accelerometer X,Y,Z are at 15,17,19.
	// Scale raw acceleration to g before sending it to Cemu's motion handler.
	// GamePad raw counts are approximately 8192 per g (observed ~8359 at rest).
	const sint16 rawX = static_cast<sint16>(report[15] | uint16(report[16]) << 8);
	const sint16 rawY = static_cast<sint16>(report[17] | uint16(report[18]) << 8);
	const sint16 rawZ = static_cast<sint16>(report[19] | uint16(report[20]) << 8);

	const sint32 roll = DecodeSigned24(report.data() + 21);
	const sint32 yaw = DecodeSigned24(report.data() + 24);
	const sint32 pitch = DecodeSigned24(report.data() + 27);
	constexpr float gyroScale = ((200.0f * 6.0f) / 154000.0f) *
		(std::numbers::pi_v<float> / 180.0f);

	return {
		.acceleration = {-rawX / 8192.0f, rawY / 8192.0f, rawZ / 8192.0f},
		// Gyro reports store roll,yaw,pitch at bytes 21,24,27.
		// Physical rotation about accel X,Y,Z corresponds to roll,yaw,pitch.
		// Keep the gyro axes aligned with their corresponding physical rotations.
		.gyroscope = {roll * gyroScale, yaw * gyroScale, pitch * gyroScale},
	};
}

// The first two bytes of a Wii U GamePad report encode the packet sequence.
// Try both byte orders; a normal adjacent report must have a delta of 1.
// 0 means duplicate; 0xffff means discontinuity.
constexpr uint16 SwapSequenceBytes(uint16 v)
{
	return static_cast<uint16>((v << 8) | (v >> 8));
}
constexpr uint16 SelectMotionReportGap(uint16 previous, uint16 current)
{
	const uint16 big = static_cast<uint16>(current - previous);
	const uint16 little = static_cast<uint16>(SwapSequenceBytes(current) - SwapSequenceBytes(previous));
	const bool bigValid = big >= 1 && big <= 180;
	const bool littleValid = little >= 1 && little <= 180;
	if (bigValid && littleValid) return std::min(big, little);
	if (bigValid) return big;
	if (littleValid) return little;
	return big == 0 ? 0 : 0xffff;
}
static_assert(SelectMotionReportGap(0x1203, 0x1204) == 1); // second byte increments
static_assert(SelectMotionReportGap(0x1203, 0x1303) == 1); // first byte increments
static_assert(SelectMotionReportGap(0x12ff, 0x1300) == 1); // big-endian wrap
static_assert(SelectMotionReportGap(0xff12, 0x0013) == 1); // little-endian wrap
static_assert(SelectMotionReportGap(0x1203, 0x1203) == 0); // duplicate

// Optional correction read once at the first motion sample. The default retains
// the hardware-derived 1x scale.
float ReadBaristaGyroGain()
{
	static const float gain = [] {
		const char* s = std::getenv("CEMU_BARISTA_GYRO_GAIN");
		if (!s || !*s) return 1.0f;
		char* end = nullptr;
		const float v = std::strtof(s, &end);
		if (s == end || *end != '\0' || !std::isfinite(v)) return 1.0f;
		return std::clamp(v, 0.25f, 16.0f);
	}();
	return gain;
}

// Apply the same proper 3D rotation to both sensors. The two +90/-90 rotations
// leave Z unchanged, preserving yaw's vertical axis. The environment variable
// is selected when Cemu starts, so changing it does not require recompilation.
enum class BaristaAxisRotation { Original, RZPlus90, RZMinus90 };

constexpr std::array<float, 3> RotateBaristaAxes(
	const std::array<float, 3>& v, BaristaAxisRotation rotation)
{
	switch (rotation)
	{
	case BaristaAxisRotation::RZPlus90: return {-v[1], v[0], v[2]};
	case BaristaAxisRotation::RZMinus90: return {v[1], -v[0], v[2]};
	default: return v;
	}
}
static_assert(RotateBaristaAxes({1.f, 2.f, 3.f}, BaristaAxisRotation::RZPlus90) ==
	std::array<float, 3>{-2.f, 1.f, 3.f});
static_assert(RotateBaristaAxes({1.f, 2.f, 3.f}, BaristaAxisRotation::RZMinus90) ==
	std::array<float, 3>{2.f, -1.f, 3.f});

// Aiming-direction correction validated with a physical GamePad in Wind Waker HD.
// After the +/-90-degree transform, GamePad physical pitch is on gyro X.
// The Y accelerometer component carries its gravity tilt near level.
// A general 3D calibration will still be needed if compound rotations drift.
constexpr void InvertBaristaPitch(std::array<float, 3>& gyro,
	std::array<float, 3>& accel)
{
	gyro[0] = -gyro[0];
	accel[1] = -accel[1];
}
constexpr bool ValidateBaristaPitchInvert()
{
	std::array<float, 3> gyro{1.f, 2.f, 3.f};
	std::array<float, 3> accel{4.f, 5.f, 6.f};
	InvertBaristaPitch(gyro, accel);
	return gyro == std::array<float, 3>{-1.f, 2.f, 3.f} &&
		accel == std::array<float, 3>{4.f, -5.f, 6.f};
}
static_assert(ValidateBaristaPitchInvert());

// Correct reversed left/right roll. After the +/-90-degree transform, physical
// roll is on gyro Y and its matching gravity tilt is on accelerometer X. Invert
// both so the two sensor inputs remain coherent for Cemu's motion fusion.
constexpr void InvertBaristaRoll(std::array<float, 3>& gyro,
	std::array<float, 3>& accel)
{
	gyro[1] = -gyro[1];
	accel[0] = -accel[0];
}
constexpr bool ValidateBaristaRollInvert()
{
	std::array<float, 3> gyro{1.f, 2.f, 3.f};
	std::array<float, 3> accel{4.f, 5.f, 6.f};
	InvertBaristaRoll(gyro, accel);
	return gyro == std::array<float, 3>{1.f, -2.f, 3.f} &&
		accel == std::array<float, 3>{-4.f, 5.f, 6.f};
}
static_assert(ValidateBaristaRollInvert());

// Correct reversed left/right yaw. Gravity supplies no absolute yaw reference,
// so only the Z gyroscope channel is inverted; there is no corresponding
// accelerometer component to change.
constexpr void InvertBaristaYaw(std::array<float, 3>& gyro)
{
	gyro[2] = -gyro[2];
}
constexpr bool ValidateBaristaYawInvert()
{
	std::array<float, 3> gyro{1.f, 2.f, 3.f};
	InvertBaristaYaw(gyro);
	return gyro == std::array<float, 3>{1.f, 2.f, -3.f};
}
static_assert(ValidateBaristaYawInvert());

bool ReadBaristaInvertPitch()
{
	static const bool enabled = [] {
		const char* setting = std::getenv("CEMU_BARISTA_INVERT_PITCH");
		if (!setting) return true;
		const std::string_view value(setting);
		return value != "0" && value != "false" && value != "off";
	}();
	return enabled;
}

bool ReadBaristaInvertRoll()
{
	static const bool enabled = [] {
		const char* setting = std::getenv("CEMU_BARISTA_INVERT_ROLL");
		if (!setting) return true;
		const std::string_view value(setting);
		return value != "0" && value != "false" && value != "off";
	}();
	return enabled;
}

bool ReadBaristaInvertYaw()
{
	static const bool enabled = [] {
		const char* setting = std::getenv("CEMU_BARISTA_INVERT_YAW");
		if (!setting) return true;
		const std::string_view value(setting);
		return value != "0" && value != "false" && value != "off";
	}();
	return enabled;
}

BaristaAxisRotation ReadBaristaAxisRotation()
{
	static const BaristaAxisRotation rotation = [] {
		const char* setting = std::getenv("CEMU_BARISTA_AXIS_ROTATION");
		if (!setting) return BaristaAxisRotation::RZPlus90;
		const std::string_view value(setting);
		if (value == "original" || value == "none") return BaristaAxisRotation::Original;
		if (value == "rz-90") return BaristaAxisRotation::RZMinus90;
		return BaristaAxisRotation::RZPlus90; // "rz+90" or unset
	}();
	return rotation;
}


struct BaristaPitchOffset
{
	float sin = 0.0f;
	float cos = 1.0f;
	bool active = false;
};

const BaristaPitchOffset& ReadBaristaPitchOffset()
{
	static const BaristaPitchOffset offset = [] {
		BaristaPitchOffset result;
		const char* s = std::getenv("CEMU_BARISTA_PITCH_OFFSET");
		if (!s || !*s) return result;
		char* end = nullptr;
		const float degrees = std::strtof(s, &end);
		if (s == end || *end != '\0' || !std::isfinite(degrees)) return result;
		const float radians = std::clamp(degrees, -90.0f, 90.0f) *
		(std::numbers::pi_v<float> / 180.0f);
		result.sin = std::sin(radians);
		result.cos = std::cos(radians);
		result.active = degrees != 0.0f;
		return result;
	}();
	return offset;
}

constexpr void ApplyBaristaPitchOffset(std::array<float, 3>& v, float sinA, float cosA)
{
	const float y = v[1];
	const float z = v[2];
	v[1] = y * cosA - z * sinA;
	v[2] = y * sinA + z * cosA;
}
constexpr bool ValidateBaristaPitchOffset()
{
	std::array<float, 3> v{4.f, 0.f, -1.f};
	ApplyBaristaPitchOffset(v, 1.f, 0.f); // +90 degrees
	return v == std::array<float, 3>{4.f, 1.f, 0.f};
}
static_assert(ValidateBaristaPitchOffset());

constexpr bool ValidateMotionDecoder()
{
	std::array<uint8, 128> report{};
	report[15] = 0x00; report[16] = 0x20; // X = +8192 (~1 g)
	report[17] = 0x00; report[18] = 0xe0; // Y = -8192 (~-1 g)
	report[19] = 0x00; report[20] = 0x20; // Z = +8192 (~1 g)
	report[21] = 0x01;                    // positive roll
	report[24] = 0xff; report[25] = 0xff; report[26] = 0xff; // yaw = -1
	report[27] = 0x01;                    // positive pitch
	const auto motion = DecodeMotion(report);
	// Independent axis fixtures: roll drives X, yaw Y, pitch Z.
	std::array<uint8, 128> pitchOnly{};
	pitchOnly[27] = 1;
	std::array<uint8, 128> yawOnly{};
	yawOnly[24] = 1;
	std::array<uint8, 128> rollOnly{};
	rollOnly[21] = 1;
	const auto p = DecodeMotion(pitchOnly);
	const auto y = DecodeMotion(yawOnly);
	const auto r = DecodeMotion(rollOnly);
	return motion.acceleration[0] == -1.0f && motion.acceleration[1] == -1.0f &&
		motion.acceleration[2] == 1.0f && motion.gyroscope[0] > 0.0f &&
		motion.gyroscope[1] < 0.0f && motion.gyroscope[2] > 0.0f &&
		p.gyroscope[0] == 0 && p.gyroscope[1] == 0 && p.gyroscope[2] > 0 &&
		y.gyroscope[0] == 0 && y.gyroscope[1] > 0 && y.gyroscope[2] == 0 &&
		r.gyroscope[0] > 0 && r.gyroscope[1] == 0 && r.gyroscope[2] == 0;
}
static_assert(ValidateMotionDecoder());
}

std::vector<std::shared_ptr<ControllerBase>> WiiUGamePadControllerProvider::get_controllers()
{
	return {std::make_shared<WiiUGamePadController>(0)};
}
bool WiiUGamePadControllerProvider::is_connected(size_t index) const
{
	std::array<uint8, 128> report{};
	return index == 0 && BaristaAppHook::ReadInput(report);
}
ControllerState WiiUGamePadControllerProvider::get_state(size_t index) const
{
	ControllerState state{};
	std::array<uint8, 128> report{};
	if (index != 0 || !BaristaAppHook::ReadInput(report)) return state;
	const uint32 buttons = DecodeButtons(report);
	for (size_t i = 0; i < s_buttonMasks.size(); ++i)
		state.buttons.SetButtonState(kButton0 + i, (buttons & s_buttonMasks[i]) != 0);
	auto stick = [&](size_t offset) {
		const sint32 value = report[offset] | uint32(report[offset + 1]) << 8;
		const float normalized = std::clamp((value - 2050) / 1150.0f, -1.0f, 1.0f);
		return std::abs(normalized) < 0.1f ? 0.0f : normalized;
	};
	state.axis = {stick(6), stick(8)};
	state.rotation = {stick(10), stick(12)};
	return state;
}

MotionSample WiiUGamePadControllerProvider::get_motion_sample(size_t index)
{
	if (index != 0)
		return {};

	std::vector<std::array<uint8, 128>> reports;
	BaristaAppHook::DrainInputReports(reports);
	const uint64 connectionRevision = BaristaAppHook::GetConnectionRevision();

	std::lock_guard lock(m_motion_mutex);
	if (connectionRevision != m_connection_revision)
	{
		m_motion_handler = {};
		m_motion_sample = {};
		m_motion_sequence_valid = false;
		m_connection_revision = connectionRevision;
	}
	for (const auto& report : reports)
	{
		const uint16 sequence = uint16(report[0]) << 8 | report[1];
		uint16 elapsedReports = 1;
		if (m_motion_sequence_valid)
		{
			// Both packet layouts occur in reverse-engineering descriptions.
			// Choose the plausible forward delta; do not reset the IMU every
			// frame just because the sequence byte order is different.
			elapsedReports = SelectMotionReportGap(m_last_motion_sequence, sequence);
			if (elapsedReports == 0)
				continue;
			if (elapsedReports > 180)
			{
				m_motion_handler = {};
				m_motion_sample = {};
				elapsedReports = 1;
			}
		}

		m_last_motion_sequence = sequence;
		m_motion_sequence_valid = true;
		const auto motion = DecodeMotion(report);
		// Optional sensor-only CSV, for diagnosing orientation/signs with a
		// real Wii U GamePad. Off by default. No button or touch data saved.
		static FILE* motionCsv = []() -> FILE* {
			const char* filename = std::getenv("CEMU_BARISTA_MOTION_CSV");
			if (!filename || !*filename) return nullptr;
			FILE* file = std::fopen(filename, "w");
			if (file)
			{
				std::fprintf(file, "time_ms,sequence,gap,acc_x,acc_y,acc_z,gyro_x,gyro_y,gyro_z\n");
				std::setvbuf(file, nullptr, _IOLBF, 0);
			}
			return file;
		}();
		if (motionCsv)
		{
			const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now().time_since_epoch()).count();
			std::fprintf(motionCsv, "%lld,%u,%u,%.7g,%.7g,%.7g,%.7g,%.7g,%.7g\n",
				static_cast<long long>(ms), static_cast<unsigned>(sequence),
				static_cast<unsigned>(elapsedReports),
				motion.acceleration[0], motion.acceleration[1], motion.acceleration[2],
				motion.gyroscope[0], motion.gyroscope[1], motion.gyroscope[2]);
		}
		const float deltaTime = std::min(elapsedReports / 180.0f, 0.1f);
		const float gain = ReadBaristaGyroGain();
		const auto rotation = ReadBaristaAxisRotation();
		auto gyro = RotateBaristaAxes(motion.gyroscope, rotation);
		auto accel = RotateBaristaAxes(motion.acceleration, rotation);
		// Preserve the selected rotation while applying the three
		// independently configurable aiming-direction corrections.
		if (rotation != BaristaAxisRotation::Original && ReadBaristaInvertPitch())
			InvertBaristaPitch(gyro, accel);
		if (rotation != BaristaAxisRotation::Original && ReadBaristaInvertRoll())
			InvertBaristaRoll(gyro, accel);
		if (rotation != BaristaAxisRotation::Original && ReadBaristaInvertYaw())
			InvertBaristaYaw(gyro);
		if (const auto& offset = ReadBaristaPitchOffset(); offset.active)
		{
			ApplyBaristaPitchOffset(gyro, offset.sin, offset.cos);
			ApplyBaristaPitchOffset(accel, offset.sin, offset.cos);
		}
		m_motion_handler.processMotionSample(deltaTime,
			gyro[0] * gain, gyro[1] * gain, gyro[2] * gain,
			accel[0], accel[1], accel[2]);
		m_motion_sample = m_motion_handler.getMotionSample();
	}
	return m_motion_sample;
}
