#pragma once

#include "arc/dx12_host_adapter.hpp"

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

class ArcExternalBridge final
{
public:
    ArcExternalBridge(
        ID3D12Device* device,
        ID3D12CommandQueue* queue,
        ID3D12GraphicsCommandList* commandList,
        ID3D12Fence* fence,
        ID3D12DescriptorHeap* rtvHeap,
        ID3D12DescriptorHeap* srvHeap,
        ID3D12Resource* const* renderTargets,
        UINT renderTargetCount,
        ID3D12Resource* vertexBuffer,
        ID3D12Resource* texture);

    ~ArcExternalBridge();

    ArcExternalBridge(const ArcExternalBridge&) = delete;
    ArcExternalBridge& operator=(const ArcExternalBridge&) = delete;

    UINT SampleIterations() const noexcept;

    void OnCommandReset();
    void ObserveTransition(ID3D12Resource* resource, const D3D12_RESOURCE_BARRIER& barrier);
    void ObserveFrameUses(ID3D12Resource* renderTarget, ID3D12Resource* vertexBuffer, ID3D12Resource* texture);
    void OnCommandClosed();
    void OnSubmit();
    void OnSignal(UINT64 fenceValue);
    void OnCompletion();
    void OnPresent(HRESULT result);
    void OnFrame(double frameMs);

    bool ShouldExit() const noexcept { return shouldExit_; }
    void Finalize();

private:
    enum class Phase : std::uint8_t { Warmup, Baseline, Adaptive, Recovery, Done };

    struct Stats
    {
        std::vector<double> frames;
        std::uint64_t misses{};
        double overshootSum{};
    };

    static Microsoft::WRL::ComPtr<IDXGIAdapter3> FindAdapter(ID3D12Device* device);
    static int EnvInt(const wchar_t* name, int fallback, int minimum, int maximum);
    static std::wstring EnvString(const wchar_t* name);
    static double Percentile(std::vector<double> values, double p);
    static double MissRatio(const Stats& stats) noexcept;
    static double MeanOvershoot(const Stats& stats) noexcept;
    static void RecomputeBudgetStats(Stats& stats, double targetMs) noexcept;
    static std::string Narrow(const std::wstring& value);
    static std::string JsonEscape(const std::string& value);

    void RegisterQualityProfile();
    void AddStat(Stats& stats, double frameMs);
    void TickGovernor(double targetMs);
    void EnterPhase(Phase phase);
    void WriteReport(bool complete);
    arc::RuntimeBackendStatus Mutate(const arc::QualityActionCandidate& action, bool restore) noexcept;

    static constexpr std::uint64_t kQualityProfileId = 9101;
    static constexpr std::array<UINT, 5> kIterations{128, 80, 48, 28, 12};

    Microsoft::WRL::ComPtr<IDXGIAdapter3> adapter_;
    std::unique_ptr<arc::dx12::NativeHostAdapter> host_;
    ID3D12CommandQueue* queue_{};
    ID3D12GraphicsCommandList* commandList_{};
    ID3D12Fence* fence_{};
    ID3D12Resource* vertexBuffer_{};
    ID3D12Resource* texture_{};

    arc::QualityResourceProfile profile_{};
    UINT qualityLevel_{};

    Phase phase_{Phase::Warmup};
    std::chrono::steady_clock::time_point phaseStart_{std::chrono::steady_clock::now()};
    int measureSeconds_{20};
    int controlFrames_{16};
    int warmupSeconds_{3};
    int recoverySeconds_{12};
    double targetMs_{};
    double recoveryTargetMs_{};

    Stats warmup_{};
    Stats baseline_{};
    Stats adaptive_{};
    std::vector<double> controlWindow_{};
    std::uint64_t adaptiveTicks_{};
    std::uint64_t recoveryTicks_{};
    bool profileRegistered_{};
    bool shouldExit_{};
    bool finalized_{};
    std::filesystem::path outputPath_{L"arc-external-result.json"};
    std::wstring arcSourceSha_{};
    std::string adapterName_{};
};
