// AMD AMF encoder backend.
//
// TODO(phase-4): implement with AMD AMF SDK.
//   - Use amf::AMFContext + D3D11 device
//   - amf::AMFComponent of AMFVideoEncoderVCE_AVC / AMFVideoEncoder_HEVC
//   - SubmitInput with AMFSurface wrapping our NV12 D3D11 texture

#include "amf_encoder.h"
#include "shared/log.h"

namespace gpur::core::encode {

namespace {
class AmfEncoder final : public IEncoder {
public:
    explicit AmfEncoder(std::shared_ptr<D3dContext> ctx) : ctx_(std::move(ctx)) {}
    Result<void> initialise(const EncoderParams& p) override {
        params_ = p;
        return err(Error::not_implemented("AMF encoder (phase 4)"));
    }
    Result<void> shutdown() override { return ok(); }
    Result<void> submit(ID3D11Texture2D*, std::chrono::nanoseconds) override {
        return err(Error::not_implemented("AmfEncoder::submit"));
    }
    Result<void> request_keyframe() override { return ok(); }
    Result<std::vector<EncodedPacket>> poll() override { return std::vector<EncodedPacket>{}; }
    Result<std::vector<EncodedPacket>> flush() override { return std::vector<EncodedPacket>{}; }
    const EncoderParams& params() const noexcept override { return params_; }
    std::string_view backend_name() const noexcept override { return "AMF(stub)"; }
private:
    std::shared_ptr<D3dContext> ctx_;
    EncoderParams               params_{};
};
} // namespace

std::unique_ptr<IEncoder> make_amf_encoder(std::shared_ptr<D3dContext> ctx) {
    return std::make_unique<AmfEncoder>(std::move(ctx));
}

} // namespace gpur::core::encode
