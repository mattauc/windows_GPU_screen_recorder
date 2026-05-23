// Intel Quick Sync (oneVPL) encoder backend.
//
// TODO(phase-4): implement with the oneVPL SDK.
//   - MFXLoad, MFXCreateSession on the chosen Intel adapter
//   - MFXVideoENCODE_Init with mfxVideoParam (AVC/HEVC)
//   - SyncOperation on the EncodeFrameAsync result

#include "qsv_encoder.h"
#include "shared/log.h"

namespace gpur::core::encode {

namespace {
class QsvEncoder final : public IEncoder {
public:
    explicit QsvEncoder(std::shared_ptr<D3dContext> ctx) : ctx_(std::move(ctx)) {}
    Result<void> initialise(const EncoderParams& p) override {
        params_ = p;
        return err(Error::not_implemented("QSV encoder (phase 4)"));
    }
    Result<void> shutdown() override { return ok(); }
    Result<void> submit(ID3D11Texture2D*, std::chrono::nanoseconds) override {
        return err(Error::not_implemented("QsvEncoder::submit"));
    }
    Result<void> request_keyframe() override { return ok(); }
    Result<std::vector<EncodedPacket>> poll() override { return std::vector<EncodedPacket>{}; }
    Result<std::vector<EncodedPacket>> flush() override { return std::vector<EncodedPacket>{}; }
    const EncoderParams& params() const noexcept override { return params_; }
    std::string_view backend_name() const noexcept override { return "QSV(stub)"; }
private:
    std::shared_ptr<D3dContext> ctx_;
    EncoderParams               params_{};
};
} // namespace

std::unique_ptr<IEncoder> make_qsv_encoder(std::shared_ptr<D3dContext> ctx) {
    return std::make_unique<QsvEncoder>(std::move(ctx));
}

} // namespace gpur::core::encode
