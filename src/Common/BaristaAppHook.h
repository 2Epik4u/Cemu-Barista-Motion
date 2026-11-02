#pragma once
#include <array>
#include <span>
#include <string>
#include <vector>

namespace BaristaAppHook
{
struct Status
{
	bool enabled = false;
	bool connected = false;
	bool socketFileExists = false;
	bool gameActive = false;
	std::string effectiveSocketPath;
	std::string configuredSocketPath;
	uint64 framesSent = 0;
	uint64 audioChunksSent = 0;
	uint64 inputReportsReceived = 0;
	sint64 lastInputMsAgo = -1;
	std::string rejectionReason;
	std::string lockHolder;
};

void Initialize(std::vector<uint8> idleRgb, unsigned width, unsigned height, const std::string& customSocketPath = "", bool enabled = true);
void Shutdown();
void SetGameActive(bool active);
bool WantsFrame();
void SubmitFrame(std::vector<uint8> rgb, unsigned width, unsigned height);
void SubmitAudio(std::span<const sint16> samples, unsigned channels);
bool ReadInput(std::array<uint8, 128>& report);
// Drains the ordered reports received since the previous call. This preserves
// the GamePad's 180 Hz motion stream while ReadInput continues to expose the
// latest report for buttons, sticks, and touch.
size_t DrainInputReports(std::vector<std::array<uint8, 128>>& reports);
// Changes whenever the AppHook socket establishes a new session. Consumers can
// use this to reset stateful input filters after a GamePad reconnect.
uint64 GetConnectionRevision();
// Reads one GamePad touchscreen contact in normalized screen coordinates.
bool ReadTouch(float& x, float& y);

std::string GetDefaultSocketPath();
std::string GetEffectiveSocketPath();
bool IsConnected();
Status GetStatus();
void Reconfigure(const std::string& customSocketPath, bool enabled);
void Reconnect();
}
