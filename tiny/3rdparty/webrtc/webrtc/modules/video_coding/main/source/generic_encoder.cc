/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/base/checks.h"
#include "webrtc/engine_configurations.h"
#include "webrtc/modules/video_coding/main/source/encoded_frame.h"
#include "webrtc/modules/video_coding/main/source/generic_encoder.h"
#include "webrtc/modules/video_coding/main/source/media_optimization.h"
#include "webrtc/system_wrappers/include/critical_section_wrapper.h"
#include "webrtc/system_wrappers/include/logging.h"

namespace webrtc {
namespace {
// Map information from info into rtp. If no relevant information is found
// in info, rtp is set to NULL.
void CopyCodecSpecific(const CodecSpecificInfo* info, RTPVideoHeader* rtp) {
  RTC_DCHECK(info);
  switch (info->codecType) {
    case kVideoCodecVP8: {
      rtp->codec = kRtpVideoVp8;
      rtp->codecHeader.VP8.InitRTPVideoHeaderVP8();
      rtp->codecHeader.VP8.pictureId = info->codecSpecific.VP8.pictureId;
      rtp->codecHeader.VP8.nonReference =
          info->codecSpecific.VP8.nonReference;
      rtp->codecHeader.VP8.temporalIdx = info->codecSpecific.VP8.temporalIdx;
      rtp->codecHeader.VP8.layerSync = info->codecSpecific.VP8.layerSync;
      rtp->codecHeader.VP8.tl0PicIdx = info->codecSpecific.VP8.tl0PicIdx;
      rtp->codecHeader.VP8.keyIdx = info->codecSpecific.VP8.keyIdx;
      rtp->simulcastIdx = info->codecSpecific.VP8.simulcastIdx;
      return;
    }
    case kVideoCodecVP9: {
      rtp->codec = kRtpVideoVp9;
      rtp->codecHeader.VP9.InitRTPVideoHeaderVP9();
      rtp->codecHeader.VP9.inter_pic_predicted =
          info->codecSpecific.VP9.inter_pic_predicted;
      rtp->codecHeader.VP9.flexible_mode =
          info->codecSpecific.VP9.flexible_mode;
      rtp->codecHeader.VP9.ss_data_available =
          info->codecSpecific.VP9.ss_data_available;
      rtp->codecHeader.VP9.picture_id = info->codecSpecific.VP9.picture_id;
      rtp->codecHeader.VP9.tl0_pic_idx = info->codecSpecific.VP9.tl0_pic_idx;
      rtp->codecHeader.VP9.temporal_idx = info->codecSpecific.VP9.temporal_idx;
      rtp->codecHeader.VP9.spatial_idx = info->codecSpecific.VP9.spatial_idx;
      rtp->codecHeader.VP9.temporal_up_switch =
          info->codecSpecific.VP9.temporal_up_switch;
      rtp->codecHeader.VP9.inter_layer_predicted =
          info->codecSpecific.VP9.inter_layer_predicted;
      rtp->codecHeader.VP9.gof_idx = info->codecSpecific.VP9.gof_idx;

      // Packetizer needs to know the number of spatial layers to correctly set
      // the marker bit, even when the number won't be written in the packet.
      rtp->codecHeader.VP9.num_spatial_layers =
          info->codecSpecific.VP9.num_spatial_layers;
      if (info->codecSpecific.VP9.ss_data_available) {
        rtp->codecHeader.VP9.spatial_layer_resolution_present =
            info->codecSpecific.VP9.spatial_layer_resolution_present;
        if (info->codecSpecific.VP9.spatial_layer_resolution_present) {
          for (size_t i = 0; i < info->codecSpecific.VP9.num_spatial_layers;
               ++i) {
            rtp->codecHeader.VP9.width[i] = info->codecSpecific.VP9.width[i];
            rtp->codecHeader.VP9.height[i] = info->codecSpecific.VP9.height[i];
          }
        }
        rtp->codecHeader.VP9.gof.CopyGofInfoVP9(info->codecSpecific.VP9.gof);
      }
      return;
    }
    case kVideoCodecH264:
      rtp->codec = kRtpVideoH264;
      return;
    case kVideoCodecGeneric:
      rtp->codec = kRtpVideoGeneric;
      rtp->simulcastIdx = info->codecSpecific.generic.simulcast_idx;
      return;
    default:
      return;
  }
}
}  // namespace

//#define DEBUG_ENCODER_BIT_STREAM

VCMGenericEncoder::VCMGenericEncoder(
    VideoEncoder* encoder,
    VideoEncoderRateObserver* rate_observer,
    VCMEncodedFrameCallback* encoded_frame_callback,
    bool internalSource)
    : encoder_(encoder),
      rate_observer_(rate_observer),
      vcm_encoded_frame_callback_(encoded_frame_callback),
      internal_source_(internalSource),
      encoder_params_({0, 0, 0, 0}),
      rotation_(kVideoRotation_0),
      is_screenshare_(false) {}

VCMGenericEncoder::~VCMGenericEncoder() {}

int32_t VCMGenericEncoder::Release() {
  return encoder_->Release();
}

int32_t VCMGenericEncoder::InitEncode(const VideoCodec* settings,
                                      int32_t numberOfCores,
                                      size_t maxPayloadSize) {
  {
    rtc::CritScope lock(&params_lock_);
    encoder_params_.target_bitrate = settings->startBitrate * 1000;
    encoder_params_.input_frame_rate = settings->maxFramerate;
  }

  is_screenshare_ = settings->mode == VideoCodecMode::kScreensharing;
  if (encoder_->InitEncode(settings, numberOfCores, maxPayloadSize) != 0) {
    LOG(LS_ERROR) << "Failed to initialize the encoder associated with "
                     "payload name: "
                  << settings->plName;
    return -1;
  }
  encoder_->RegisterEncodeCompleteCallback(vcm_encoded_frame_callback_);
  return 0;
}

int32_t VCMGenericEncoder::Encode(const VideoFrame& inputFrame,
                                  const CodecSpecificInfo* codecSpecificInfo,
                                  const std::vector<FrameType>& frameTypes) {
  for (FrameType frame_type : frameTypes)
    RTC_DCHECK(frame_type == kVideoFrameKey || frame_type == kVideoFrameDelta);

  rotation_ = inputFrame.rotation();

  // Keep track of the current frame rotation and apply to the output of the
  // encoder. There might not be exact as the encoder could have one frame delay
  // but it should be close enough.
  // TODO(pbos): Map from timestamp, this is racy (even if rotation_ is locked
  // properly, which it isn't). More than one frame may be in the pipeline.
  vcm_encoded_frame_callback_->SetRotation(rotation_);

  int32_t result = encoder_->Encode(inputFrame, codecSpecificInfo, &frameTypes);
  if (is_screenshare_ &&
      result == WEBRTC_VIDEO_CODEC_TARGET_BITRATE_OVERSHOOT) {
    // Target bitrate exceeded, encoder state has been reset - try again.
    return encoder_->Encode(inputFrame, codecSpecificInfo, &frameTypes);
  }

  return result;
}

void VCMGenericEncoder::SetEncoderParameters(const EncoderParameters& params) {
  bool channel_parameters_have_changed;
  bool rates_have_changed;
  {
    rtc::CritScope lock(&params_lock_);
    channel_parameters_have_changed =
        params.loss_rate != encoder_params_.loss_rate ||
        params.rtt != encoder_params_.rtt;
    rates_have_changed =
        params.target_bitrate != encoder_params_.target_bitrate ||
        params.input_frame_rate != encoder_params_.input_frame_rate;
    encoder_params_ = params;
  }
  if (channel_parameters_have_changed)
    encoder_->SetChannelParameters(params.loss_rate, params.rtt);
  if (rates_have_changed) {
    uint32_t target_bitrate_kbps = (params.target_bitrate + 500) / 1000;
    encoder_->SetRates(target_bitrate_kbps, params.input_frame_rate);
    if (rate_observer_ != nullptr) {
      rate_observer_->OnSetRates(params.target_bitrate,
                                 params.input_frame_rate);
    }
  }
}

EncoderParameters VCMGenericEncoder::GetEncoderParameters() const {
  rtc::CritScope lock(&params_lock_);
  return encoder_params_;
}

int32_t
VCMGenericEncoder::SetPeriodicKeyFrames(bool enable)
{
    return encoder_->SetPeriodicKeyFrames(enable);
}

int32_t VCMGenericEncoder::RequestFrame(
    const std::vector<FrameType>& frame_types) {
  VideoFrame image;
  return encoder_->Encode(image, NULL, &frame_types);
}

bool
VCMGenericEncoder::InternalSource() const
{
    return internal_source_;
}

void VCMGenericEncoder::OnDroppedFrame() {
  encoder_->OnDroppedFrame();
}

bool VCMGenericEncoder::SupportsNativeHandle() const {
  return encoder_->SupportsNativeHandle();
}

int VCMGenericEncoder::GetTargetFramerate() {
  return encoder_->GetTargetFramerate();
}

 /***************************
  * Callback Implementation
  ***************************/
VCMEncodedFrameCallback::VCMEncodedFrameCallback(
    EncodedImageCallback* post_encode_callback)
    : _sendCallback(),
      _mediaOpt(NULL),
      _payloadType(0),
      _internalSource(false),
      _rotation(kVideoRotation_0),
      post_encode_callback_(post_encode_callback)
#ifdef DEBUG_ENCODER_BIT_STREAM
      ,
      _bitStreamAfterEncoder(NULL)
#endif
{
#ifdef DEBUG_ENCODER_BIT_STREAM
    _bitStreamAfterEncoder = fopen("encoderBitStream.bit", "wb");
#endif
}

VCMEncodedFrameCallback::~VCMEncodedFrameCallback()
{
#ifdef DEBUG_ENCODER_BIT_STREAM
    fclose(_bitStreamAfterEncoder);
#endif
}

int32_t
VCMEncodedFrameCallback::SetTransportCallback(VCMPacketizationCallback* transport)
{
    _sendCallback = transport;
    return VCM_OK;
}

int32_t VCMEncodedFrameCallback::Encoded(
    const EncodedImage& encodedImage,
    const CodecSpecificInfo* codecSpecificInfo,
    const RTPFragmentationHeader* fragmentationHeader) {
  RTC_DCHECK(encodedImage._frameType == kVideoFrameKey ||
             encodedImage._frameType == kVideoFrameDelta);
  post_encode_callback_->Encoded(encodedImage, NULL, NULL);

  if (_sendCallback == NULL) {
    return VCM_UNINITIALIZED;
  }

#ifdef DEBUG_ENCODER_BIT_STREAM
  if (_bitStreamAfterEncoder != NULL) {
    fwrite(encodedImage._buffer, 1, encodedImage._length,
           _bitStreamAfterEncoder);
  }
#endif

  RTPVideoHeader rtpVideoHeader;
  memset(&rtpVideoHeader, 0, sizeof(RTPVideoHeader));
  RTPVideoHeader* rtpVideoHeaderPtr = &rtpVideoHeader;
  if (codecSpecificInfo) {
    CopyCodecSpecific(codecSpecificInfo, rtpVideoHeaderPtr);
  }
  rtpVideoHeader.rotation = _rotation;

  int32_t callbackReturn = _sendCallback->SendData(
      _payloadType, encodedImage, *fragmentationHeader, rtpVideoHeaderPtr);
  if (callbackReturn < 0) {
    return callbackReturn;
  }

  if (_mediaOpt != NULL) {
    _mediaOpt->UpdateWithEncodedData(encodedImage);
    if (_internalSource)
      return _mediaOpt->DropFrame();  // Signal to encoder to drop next frame.
  }
  return VCM_OK;
}

void
VCMEncodedFrameCallback::SetMediaOpt(
    media_optimization::MediaOptimization *mediaOpt)
{
    _mediaOpt = mediaOpt;
}

}  // namespace webrtc
