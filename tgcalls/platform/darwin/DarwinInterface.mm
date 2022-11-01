#include "DarwinInterface.h"

#include "VideoCapturerInterfaceImpl.h"
#include "sdk/objc/native/src/objc_video_track_source.h"
#include "sdk/objc/native/api/network_monitor_factory.h"

#include "media/base/media_constants.h"
#include "TGRTCDefaultVideoEncoderFactory.h"
#include "TGRTCDefaultVideoDecoderFactory.h"
#include "sdk/objc/native/api/video_encoder_factory.h"
#include "sdk/objc/native/api/video_decoder_factory.h"
#include "pc/video_track_source_proxy.h"
#import "base/RTCLogging.h"
#include "AudioDeviceModuleIOS.h"
#include "AudioDeviceModuleMacos.h"
#include "DarwinVideoSource.h"
#include "objc_video_encoder_factory.h"
#include "objc_video_decoder_factory.h"

#ifdef WEBRTC_IOS
#include "platform/darwin/iOS/RTCAudioSession.h"
#include "platform/darwin/iOS/RTCAudioSessionConfiguration.h"
#import <UIKit/UIKit.h>
#endif // WEBRTC_IOS

#import <AVFoundation/AVFoundation.h>

#include <sys/sysctl.h>

namespace tgcalls {

std::unique_ptr<webrtc::VideoDecoderFactory> CustomObjCToNativeVideoDecoderFactory(
    id<RTC_OBJC_TYPE(RTCVideoDecoderFactory)> objc_video_decoder_factory) {
    return std::make_unique<webrtc::CustomObjCVideoDecoderFactory>(objc_video_decoder_factory);
}

static DarwinVideoTrackSource *getObjCVideoSource(const rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> nativeSource) {
    webrtc::VideoTrackSourceProxy *proxy_source =
    static_cast<webrtc::VideoTrackSourceProxy *>(nativeSource.get());
    return static_cast<DarwinVideoTrackSource *>(proxy_source->internal());
}

static NSString *getPlatformInfo() {
    const char *typeSpecifier = "hw.machine";
    
    size_t size;
    sysctlbyname(typeSpecifier, NULL, &size, NULL, 0);
    
    char *answer = (char *)malloc(size);
    sysctlbyname(typeSpecifier, answer, &size, NULL, 0);
    
    NSString *results = [NSString stringWithCString:answer encoding:NSUTF8StringEncoding];
    
    free(answer);
    return results;
}

std::unique_ptr<rtc::NetworkMonitorFactory> DarwinInterface::createNetworkMonitorFactory() {
    return webrtc::CreateNetworkMonitorFactory();
}

void DarwinInterface::configurePlatformAudio(int numChanels) {
#ifdef WEBRTC_IOS
    RTCAudioSessionConfiguration *sharedConfiguration = [RTCAudioSessionConfiguration webRTCConfiguration];
    sharedConfiguration.categoryOptions |= AVAudioSessionCategoryOptionMixWithOthers;
    sharedConfiguration.outputNumberOfChannels = numChanels;
    [RTCAudioSessionConfiguration setWebRTCConfiguration:sharedConfiguration];

    /*[RTCAudioSession sharedInstance].useManualAudio = true;
    [[RTCAudioSession sharedInstance] audioSessionDidActivate:[AVAudioSession sharedInstance]];
    [RTCAudioSession sharedInstance].isAudioEnabled = true;*/
    
    RTCLogInfo(@"Configuring platform: %@ %@", getPlatformInfo(), [[UIDevice currentDevice] systemVersion]);
#endif
}

std::unique_ptr<webrtc::VideoEncoderFactory> DarwinInterface::makeVideoEncoderFactory(bool preferHardwareEncoding, bool isScreencast) {
    auto nativeFactory = std::make_unique<webrtc::CustomObjCVideoEncoderFactory>([[TGRTCDefaultVideoEncoderFactory alloc] initWithPreferHardwareH264:preferHardwareEncoding preferX264:false]);
    if (!preferHardwareEncoding && !isScreencast) {
        auto nativeHardwareFactory = std::make_unique<webrtc::CustomObjCVideoEncoderFactory>([[TGRTCDefaultVideoEncoderFactory alloc] initWithPreferHardwareH264:true preferX264:false]);
        return std::make_unique<webrtc::SimulcastVideoEncoderFactory>(std::move(nativeFactory), std::move(nativeHardwareFactory));
    }
    return nativeFactory;
}

std::unique_ptr<webrtc::VideoDecoderFactory> DarwinInterface::makeVideoDecoderFactory() {
    return CustomObjCToNativeVideoDecoderFactory([[TGRTCDefaultVideoDecoderFactory alloc] init]);
}

bool DarwinInterface::supportsEncoding(const std::string &codecName) {
    if (false) {
    }
#ifndef WEBRTC_DISABLE_H265
    else if (codecName == cricket::kH265CodecName) {
#ifdef WEBRTC_IOS
		if (@available(iOS 11.0, *)) {
			return [[AVAssetExportSession allExportPresets] containsObject:AVAssetExportPresetHEVCHighestQuality];
		}
#elif defined WEBRTC_MAC // WEBRTC_IOS
        
#ifdef __x86_64__
        return NO;
#else
        return YES;
#endif
#endif // WEBRTC_IOS || WEBRTC_MAC
    }
#endif
    else if (codecName == cricket::kH264CodecName) {
#ifdef __x86_64__
        return YES;
#else
        return NO;
#endif
    } else if (codecName == cricket::kVp8CodecName) {
        return true;
    } else if (codecName == cricket::kVp9CodecName) {
        return true;
    }
    return false;
}

rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> DarwinInterface::makeVideoSource(rtc::Thread *signalingThread, rtc::Thread *workerThread) {
    rtc::scoped_refptr<tgcalls::DarwinVideoTrackSource> objCVideoTrackSource(new rtc::RefCountedObject<tgcalls::DarwinVideoTrackSource>());
    return webrtc::VideoTrackSourceProxy::Create(signalingThread, workerThread, objCVideoTrackSource);
}

void DarwinInterface::adaptVideoSource(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> videoSource, int width, int height, int fps) {
    getObjCVideoSource(videoSource)->OnOutputFormatRequest(width, height, fps);
}

std::unique_ptr<VideoCapturerInterface> DarwinInterface::makeVideoCapturer(rtc::scoped_refptr<webrtc::VideoTrackSourceInterface> source, std::string deviceId, std::function<void(VideoState)> stateUpdated, std::function<void(PlatformCaptureInfo)> captureInfoUpdated, std::shared_ptr<PlatformContext> platformContext, std::pair<int, int> &outResolution) {
    return std::make_unique<VideoCapturerInterfaceImpl>(source, deviceId, stateUpdated, captureInfoUpdated, outResolution);
}

rtc::scoped_refptr<WrappedAudioDeviceModule> DarwinInterface::wrapAudioDeviceModule(rtc::scoped_refptr<webrtc::AudioDeviceModule> module) {
#ifdef WEBRTC_MAC
    return rtc::make_ref_counted<AudioDeviceModuleMacos>(module);
#else
    return rtc::make_ref_counted<AudioDeviceModuleIOS>(module);
#endif
}

std::unique_ptr<PlatformInterface> CreatePlatformInterface() {
	return std::make_unique<DarwinInterface>();
}

} // namespace tgcalls
