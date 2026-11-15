#pragma once
#include "camera-policy.h"
#include "api/video/i420_buffer.h"
#include "libyuv.h"
#include <mfapi.h>
namespace camera {
inline GUID YUVOutputSubtype(REFGUID native) {
  return native==MFVideoFormat_YUY2?MFVideoFormat_YUY2:MFVideoFormat_NV12;
}
inline webrtc::scoped_refptr<webrtc::I420Buffer> ConvertYUV(const uint8_t* data,size_t length,int width,int height,int stride,bool nv12) {
  if(!data||width<2||height<2||width%2||height%2||stride<width*(nv12?1:2))return nullptr;
  const uint64_t required=uint64_t(stride)*(nv12?height+height/2:height);
  if(length<required)return nullptr;
  auto frame=webrtc::I420Buffer::Create(width,height);
  int result=nv12?
    libyuv::NV12ToI420(data,stride,data+size_t(stride)*height,stride,frame->MutableDataY(),frame->StrideY(),frame->MutableDataU(),frame->StrideU(),frame->MutableDataV(),frame->StrideV(),width,height):
    libyuv::YUY2ToI420(data,stride,frame->MutableDataY(),frame->StrideY(),frame->MutableDataU(),frame->StrideU(),frame->MutableDataV(),frame->StrideV(),width,height);
  if(result)return nullptr;
  auto size=WebSize(width,height);
  if(size.width==width&&size.height==height)return frame;
  auto resized=webrtc::I420Buffer::Create(size.width,size.height);
  if(libyuv::I420Scale(frame->DataY(),frame->StrideY(),frame->DataU(),frame->StrideU(),frame->DataV(),frame->StrideV(),width,height,resized->MutableDataY(),resized->StrideY(),resized->MutableDataU(),resized->StrideU(),resized->MutableDataV(),resized->StrideV(),size.width,size.height,libyuv::kFilterBox))return nullptr;
  return resized;
}
inline webrtc::scoped_refptr<webrtc::I420Buffer> ConvertRGB(const uint8_t* data,size_t length,int width,int height,int stride) {
  if(!data||width<2||height<2||std::abs(stride)<width*4||length<uint64_t(std::abs(stride))*height)return nullptr;
  auto frame=webrtc::I420Buffer::Create(width,height);
  if(libyuv::ARGBToI420(data,std::abs(stride),frame->MutableDataY(),frame->StrideY(),frame->MutableDataU(),frame->StrideU(),frame->MutableDataV(),frame->StrideV(),width,stride<0?-height:height))return nullptr;
  auto size=WebSize(width,height);
  if(size.width==width&&size.height==height)return frame;
  auto resized=webrtc::I420Buffer::Create(size.width,size.height);
  if(libyuv::I420Scale(frame->DataY(),frame->StrideY(),frame->DataU(),frame->StrideU(),frame->DataV(),frame->StrideV(),width,height,resized->MutableDataY(),resized->StrideY(),resized->MutableDataU(),resized->StrideU(),resized->MutableDataV(),resized->StrideV(),size.width,size.height,libyuv::kFilterBox))return nullptr;
  return resized;
}
}
