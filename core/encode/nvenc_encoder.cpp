// NVENC encoder backend.
//
// Phase 0 implementation. Uses NVIDIA Video Codec SDK 12.x.
//
// Key design points:
//   * We pass D3D11 textures directly into NVENC via NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX.
//   * Synchronous mode (enableEncodeAsync = 0). Simplest to reason about; we can switch
//     to async (event-driven) mode in a later phase for an extra ~10-15% throughput.
//   * Pre-register a small ring of NV12 input textures so submit() avoids the
//     register/unregister cost on the hot path.
//   * Output is Annex-B framed NALUs (the muxer / Phase 0 file writer expects this).

#include "nvenc_encoder.h"
#include "shared/log.h"

#include <atomic>
#include <cstring>
#include <mutex>

#if GPUR_HAVE_NVENC
  #include "nvEncodeAPI.h"
  #include <windows.h>
#endif

namespace gpur::core::encode {

namespace {

#if GPUR_HAVE_NVENC

constexpr uint32_t kInputRingSize     = 4;
constexpr uint32_t kBitstreamRingSize = 8;
constexpr uint32_t kMaxBitstreamBytes = 8 * 1024 * 1024;  // 8 MB per packet ceiling

// Wrap a couple of GUIDs we use a lot.
constexpr GUID codec_guid(Codec c) {
    return c == Codec::HEVC ? NV_ENC_CODEC_HEVC_GUID
         : c == Codec::AV1  ? NV_ENC_CODEC_AV1_GUID
         :                    NV_ENC_CODEC_H264_GUID;
}

constexpr GUID preset_guid_low_latency() {
    // P4 = balanced quality/perf; LOW_LATENCY tuning. Matches ShadowPlay defaults.
    return NV_ENC_PRESET_P4_GUID;
}

class NvencEncoder final : public IEncoder {
public:
    explicit NvencEncoder(std::shared_ptr<D3dContext> ctx) : ctx_(std::move(ctx)) {}

    ~NvencEncoder() override {
        (void)shutdown();
    }

    Result<void> initialise(const EncoderParams& params) override {
        if (encoder_) {
            return err(Error::make(Error::Code::AlreadyInitialised, "NVENC already initialised"));
        }
        params_ = params;

        if (auto r = load_dll(); !r) return r;

        // Open session.
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS sp{};
        sp.version    = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
        sp.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
        sp.device     = ctx_->device();
        sp.apiVersion = NVENCAPI_VERSION;

        if (auto s = api_.nvEncOpenEncodeSessionEx(&sp, &encoder_); s != NV_ENC_SUCCESS) {
            return nvenc_error("nvEncOpenEncodeSessionEx", s);
        }

        // Configure.
        NV_ENC_INITIALIZE_PARAMS ip{};
        ip.version          = NV_ENC_INITIALIZE_PARAMS_VER;
        ip.encodeGUID       = codec_guid(params.codec);
        ip.presetGUID       = preset_guid_low_latency();
        ip.encodeWidth      = params.width;
        ip.encodeHeight     = params.height;
        ip.darWidth         = params.width;
        ip.darHeight        = params.height;
        ip.frameRateNum     = params.fps;
        ip.frameRateDen     = 1;
        ip.enablePTD        = 1;                           // let NVENC pick frame types
        ip.maxEncodeWidth   = params.width;
        ip.maxEncodeHeight  = params.height;
        ip.enableEncodeAsync = 0;
        ip.tuningInfo       = params.low_latency
            ? NV_ENC_TUNING_INFO_LOW_LATENCY
            : NV_ENC_TUNING_INFO_HIGH_QUALITY;

        // Pull the preset's default config and override fields we care about.
        NV_ENC_PRESET_CONFIG preset_cfg{};
        preset_cfg.version           = NV_ENC_PRESET_CONFIG_VER;
        preset_cfg.presetCfg.version = NV_ENC_CONFIG_VER;
        if (auto s = api_.nvEncGetEncodePresetConfigEx(encoder_, ip.encodeGUID,
                                                       ip.presetGUID,
                                                       ip.tuningInfo,
                                                       &preset_cfg); s != NV_ENC_SUCCESS) {
            return nvenc_error("nvEncGetEncodePresetConfigEx", s);
        }
        cfg_ = preset_cfg.presetCfg;

        cfg_.gopLength            = params.keyframe_interval_frames;
        cfg_.frameIntervalP       = static_cast<int>(params.bframes) + 1;
        cfg_.rcParams.rateControlMode =
            params.rate_control == RateControl::ConstantBitrate
                ? NV_ENC_PARAMS_RC_CBR
            : params.rate_control == RateControl::VariableBitrate
                ? NV_ENC_PARAMS_RC_VBR
                : NV_ENC_PARAMS_RC_CONSTQP;
        cfg_.rcParams.averageBitRate = params.bitrate_bps;
        cfg_.rcParams.maxBitRate     = params.bitrate_bps;
        cfg_.rcParams.vbvBufferSize  = params.bitrate_bps;        // 1s VBV
        cfg_.rcParams.vbvInitialDelay = params.bitrate_bps;
        if (params.rate_control == RateControl::ConstantQuality) {
            cfg_.rcParams.constQP.qpInterP = params.cqp_quality;
            cfg_.rcParams.constQP.qpInterB = params.cqp_quality;
            cfg_.rcParams.constQP.qpIntra  = params.cqp_quality;
        }

        // Codec-specific bits.
        if (params.codec == Codec::H264) {
            auto& h = cfg_.encodeCodecConfig.h264Config;
            h.idrPeriod        = cfg_.gopLength;
            h.repeatSPSPPS     = 1;
            h.outputAUD        = 0;
            h.disableSPSPPS    = 0;
            h.sliceMode        = 0;                  // single slice per picture
            h.sliceModeData    = 0;
        } else if (params.codec == Codec::HEVC) {
            auto& h = cfg_.encodeCodecConfig.hevcConfig;
            h.idrPeriod    = cfg_.gopLength;
            h.repeatSPSPPS = 1;
            h.outputAUD    = 0;
        }

        ip.encodeConfig = &cfg_;

        if (auto s = api_.nvEncInitializeEncoder(encoder_, &ip); s != NV_ENC_SUCCESS) {
            return nvenc_error("nvEncInitializeEncoder", s);
        }

        // Pre-allocate input texture ring + register with NVENC.
        input_ring_.resize(kInputRingSize);
        for (auto& slot : input_ring_) {
            auto tex = create_nv12_input_texture(*ctx_, params.width, params.height);
            if (!tex) return err(std::move(tex.error()));
            slot.texture = *tex;

            NV_ENC_REGISTER_RESOURCE rr{};
            rr.version             = NV_ENC_REGISTER_RESOURCE_VER;
            rr.resourceType        = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
            rr.width               = params.width;
            rr.height              = params.height;
            rr.pitch               = 0;
            rr.resourceToRegister  = slot.texture.Get();
            rr.bufferFormat        = NV_ENC_BUFFER_FORMAT_NV12;
            rr.bufferUsage         = NV_ENC_INPUT_IMAGE;
            if (auto s = api_.nvEncRegisterResource(encoder_, &rr); s != NV_ENC_SUCCESS) {
                return nvenc_error("nvEncRegisterResource", s);
            }
            slot.registered = rr.registeredResource;
        }

        // Pre-allocate output bitstream ring.
        bitstream_ring_.resize(kBitstreamRingSize);
        for (auto& b : bitstream_ring_) {
            NV_ENC_CREATE_BITSTREAM_BUFFER cb{};
            cb.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
            if (auto s = api_.nvEncCreateBitstreamBuffer(encoder_, &cb); s != NV_ENC_SUCCESS) {
                return nvenc_error("nvEncCreateBitstreamBuffer", s);
            }
            b.bitstream = cb.bitstreamBuffer;
        }

        GPUR_INFO("NVENC initialised: {}x{} @ {}fps, codec={}, bitrate={}kbps",
                  params.width, params.height, params.fps,
                  params.codec == Codec::HEVC ? "HEVC" : "H.264",
                  params.bitrate_bps / 1000);

        return ok();
    }

    Result<void> shutdown() override {
        if (!encoder_) return ok();

        // Drain any in-flight packets — best-effort.
        (void)flush();

        for (auto& b : bitstream_ring_) {
            if (b.bitstream) api_.nvEncDestroyBitstreamBuffer(encoder_, b.bitstream);
        }
        bitstream_ring_.clear();

        for (auto& s : input_ring_) {
            if (s.registered) api_.nvEncUnregisterResource(encoder_, s.registered);
        }
        input_ring_.clear();

        api_.nvEncDestroyEncoder(encoder_);
        encoder_ = nullptr;

        if (nvenc_dll_) {
            FreeLibrary(nvenc_dll_);
            nvenc_dll_ = nullptr;
        }
        return ok();
    }

    Result<void> submit(ID3D11Texture2D* nv12_frame, std::chrono::nanoseconds pts) override {
        if (!encoder_) return err(Error::make(Error::Code::NotInitialised, "NVENC submit before init"));
        if (!nv12_frame) return err(Error::make(Error::Code::InvalidArgument, "submit: null texture"));

        // Copy the caller's NV12 frame into our registered ring slot. The
        // converter's output goes into our ring slot directly anyway — the
        // copy here is just a safety net for callers that hand us their own
        // texture. The fast path in pipeline.cpp asks us for an input texture
        // via acquire_input() (TODO) and writes the converter output into it,
        // avoiding the copy entirely.
        uint32_t slot_idx = (input_write_idx_++) % kInputRingSize;
        auto& slot = input_ring_[slot_idx];
        {
            std::scoped_lock g(ctx_->immediate_mutex());
            ctx_->immediate()->CopyResource(slot.texture.Get(), nv12_frame);
        }

        NV_ENC_MAP_INPUT_RESOURCE mp{};
        mp.version            = NV_ENC_MAP_INPUT_RESOURCE_VER;
        mp.registeredResource = slot.registered;
        if (auto s = api_.nvEncMapInputResource(encoder_, &mp); s != NV_ENC_SUCCESS) {
            return nvenc_error("nvEncMapInputResource", s);
        }

        uint32_t bs_idx = (bitstream_write_idx_++) % kBitstreamRingSize;
        auto& bs = bitstream_ring_[bs_idx];

        NV_ENC_PIC_PARAMS pp{};
        pp.version          = NV_ENC_PIC_PARAMS_VER;
        pp.inputWidth       = params_.width;
        pp.inputHeight      = params_.height;
        pp.inputPitch       = params_.width;
        pp.inputBuffer      = mp.mappedResource;
        pp.bufferFmt        = mp.mappedBufferFmt;
        pp.outputBitstream  = bs.bitstream;
        pp.pictureStruct    = NV_ENC_PIC_STRUCT_FRAME;
        pp.inputTimeStamp   = pts.count();
        if (force_idr_.exchange(false)) {
            pp.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;
        }

        auto encode_status = api_.nvEncEncodePicture(encoder_, &pp);
        const bool buffered = (encode_status == NV_ENC_ERR_NEED_MORE_INPUT);
        if (encode_status != NV_ENC_SUCCESS && !buffered) {
            api_.nvEncUnmapInputResource(encoder_, mp.mappedResource);
            return nvenc_error("nvEncEncodePicture", encode_status);
        }

        bs.in_flight = true;
        bs.mapped_input = mp.mappedResource;

        if (!buffered) {
            // Synchronous mode: bitstream is ready to lock immediately.
            if (auto r = drain_one(bs); !r) {
                return err(std::move(r.error()));
            }
        }
        return ok();
    }

    Result<void> request_keyframe() override {
        force_idr_.store(true);
        return ok();
    }

    Result<std::vector<EncodedPacket>> poll() override {
        std::vector<EncodedPacket> out;
        {
            std::scoped_lock g(out_mu_);
            out.swap(pending_out_);
        }
        return out;
    }

    Result<std::vector<EncodedPacket>> flush() override {
        if (!encoder_) return std::vector<EncodedPacket>{};

        // EOS picture.
        NV_ENC_PIC_PARAMS pp{};
        pp.version       = NV_ENC_PIC_PARAMS_VER;
        pp.encodePicFlags = NV_ENC_PIC_FLAG_EOS;
        api_.nvEncEncodePicture(encoder_, &pp);

        // Drain any remaining bitstream buffers that haven't been picked up.
        for (auto& bs : bitstream_ring_) {
            if (bs.in_flight) (void)drain_one(bs);
        }
        return poll();
    }

    const EncoderParams& params() const noexcept override { return params_; }
    std::string_view backend_name() const noexcept override { return "NVENC"; }

private:
    struct InputSlot {
        ComPtr<ID3D11Texture2D> texture;
        NV_ENC_REGISTERED_PTR   registered{};
    };
    struct BitstreamSlot {
        NV_ENC_OUTPUT_PTR  bitstream{};
        NV_ENC_INPUT_PTR   mapped_input{};
        bool               in_flight{false};
    };

    Result<void> load_dll() {
        nvenc_dll_ = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!nvenc_dll_) {
            return err(Error::from_hresult(HRESULT_FROM_WIN32(GetLastError()),
                                            "LoadLibrary(nvEncodeAPI64.dll) failed"));
        }
        using CreatePfn = NVENCSTATUS (NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);
        auto create = reinterpret_cast<CreatePfn>(
            GetProcAddress(nvenc_dll_, "NvEncodeAPICreateInstance"));
        if (!create) {
            return err(Error::make(Error::Code::EncoderFailed,
                                    "NvEncodeAPICreateInstance not exported by nvEncodeAPI64.dll"));
        }
        api_.version = NV_ENCODE_API_FUNCTION_LIST_VER;
        if (auto s = create(&api_); s != NV_ENC_SUCCESS) {
            return nvenc_error("NvEncodeAPICreateInstance", s);
        }
        return ok();
    }

    Result<void> drain_one(BitstreamSlot& bs) {
        NV_ENC_LOCK_BITSTREAM lock{};
        lock.version         = NV_ENC_LOCK_BITSTREAM_VER;
        lock.outputBitstream = bs.bitstream;
        if (auto s = api_.nvEncLockBitstream(encoder_, &lock); s != NV_ENC_SUCCESS) {
            return nvenc_error("nvEncLockBitstream", s);
        }

        EncodedPacket pkt;
        pkt.data.assign(
            reinterpret_cast<const uint8_t*>(lock.bitstreamBufferPtr),
            reinterpret_cast<const uint8_t*>(lock.bitstreamBufferPtr) + lock.bitstreamSizeInBytes);
        pkt.pts      = std::chrono::nanoseconds(lock.outputTimeStamp);
        pkt.dts      = std::chrono::nanoseconds(lock.outputTimeStamp - lock.outputDuration);
        pkt.keyframe = (lock.pictureType == NV_ENC_PIC_TYPE_IDR ||
                        lock.pictureType == NV_ENC_PIC_TYPE_I);

        api_.nvEncUnlockBitstream(encoder_, bs.bitstream);

        if (bs.mapped_input) {
            api_.nvEncUnmapInputResource(encoder_, bs.mapped_input);
            bs.mapped_input = nullptr;
        }
        bs.in_flight = false;

        {
            std::scoped_lock g(out_mu_);
            pending_out_.push_back(std::move(pkt));
        }
        return ok();
    }

    static std::unexpected<Error> nvenc_error(const char* fn, NVENCSTATUS s) {
        return err(Error::make(Error::Code::EncoderFailed,
                                std::string{fn} + " failed: NVENC status " + std::to_string(static_cast<int>(s))));
    }

    std::shared_ptr<D3dContext>     ctx_;
    EncoderParams                   params_;
    HMODULE                         nvenc_dll_{nullptr};
    NV_ENCODE_API_FUNCTION_LIST     api_{};
    void*                           encoder_{nullptr};
    NV_ENC_CONFIG                   cfg_{};

    std::vector<InputSlot>          input_ring_;
    std::atomic<uint32_t>           input_write_idx_{0};

    std::vector<BitstreamSlot>      bitstream_ring_;
    std::atomic<uint32_t>           bitstream_write_idx_{0};

    std::atomic<bool>               force_idr_{false};

    std::mutex                      out_mu_;
    std::vector<EncodedPacket>      pending_out_;
};

#else  // !GPUR_HAVE_NVENC

class NvencEncoder final : public IEncoder {
public:
    explicit NvencEncoder(std::shared_ptr<D3dContext>) {}
    Result<void> initialise(const EncoderParams&) override {
        return err(Error::make(Error::Code::NotImplemented,
                                "NVENC backend not compiled in (set GPUR_BUILD_NVENC=ON and supply the SDK)"));
    }
    Result<void> shutdown() override { return ok(); }
    Result<void> submit(ID3D11Texture2D*, std::chrono::nanoseconds) override {
        return err(Error::make(Error::Code::NotImplemented, "NVENC submit"));
    }
    Result<void> request_keyframe() override { return ok(); }
    Result<std::vector<EncodedPacket>> poll() override { return std::vector<EncodedPacket>{}; }
    Result<std::vector<EncodedPacket>> flush() override { return std::vector<EncodedPacket>{}; }
    const EncoderParams& params() const noexcept override { return params_; }
    std::string_view backend_name() const noexcept override { return "NVENC(stub)"; }
private:
    EncoderParams params_{};
};

#endif // GPUR_HAVE_NVENC

} // namespace

std::unique_ptr<IEncoder> make_nvenc_encoder(std::shared_ptr<D3dContext> ctx) {
    return std::make_unique<NvencEncoder>(std::move(ctx));
}

bool nvenc_is_available(std::shared_ptr<D3dContext> ctx) {
#if GPUR_HAVE_NVENC
    auto enc = make_nvenc_encoder(ctx);
    EncoderParams probe{};
    probe.width  = 1280;
    probe.height = 720;
    auto r = enc->initialise(probe);
    if (!r) return false;
    enc->shutdown();
    return true;
#else
    (void)ctx;
    return false;
#endif
}

Result<ComPtr<ID3D11Texture2D>> create_nv12_input_texture(
    D3dContext& ctx, uint32_t width, uint32_t height) {
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = width;
    td.Height           = height;
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    // UAV so the BGRA→NV12 compute shader can write directly into it;
    // SHADER_RESOURCE so NVENC can sample / read it.
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    td.MiscFlags        = 0;

    ComPtr<ID3D11Texture2D> tex;
    if (HRESULT hr = ctx.device()->CreateTexture2D(&td, nullptr, &tex); FAILED(hr)) {
        return err(Error::from_hresult(hr, "CreateTexture2D(NV12 encoder input) failed"));
    }
    return tex;
}

// Default factory. For now there's only NVENC; AMF/QSV will be added in Phase 4.
Result<std::unique_ptr<IEncoder>> make_encoder(std::shared_ptr<D3dContext> ctx) {
#if GPUR_HAVE_NVENC
    return make_nvenc_encoder(std::move(ctx));
#else
    return err(Error::make(Error::Code::NotImplemented,
                            "no encoder backend compiled in"));
#endif
}

} // namespace gpur::core::encode
