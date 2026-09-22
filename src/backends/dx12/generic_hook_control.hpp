#pragma once
namespace arc::dx12::hooks {
// Changes native hook installation only at bounded discovery/VRS transitions,
// never once per draw. Balanced begin/end supports overlapping diagnostic setup.
bool begin_raster_observation() noexcept;
void end_raster_observation() noexcept;
void request_passive_when_idle() noexcept;
}
