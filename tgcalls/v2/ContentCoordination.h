#ifndef TGCALLS_CONTENT_COORDINATION_H
#define TGCALLS_CONTENT_COORDINATION_H

#include <memory>

#include "pc/channel_manager.h"
#include "pc/media_session.h"
#include "pc/session_description.h"
#include "p2p/base/transport_description_factory.h"

#include "v2/Signaling.h"

namespace tgcalls {

class ContentCoordinationContext {
public:
    struct NegotiationContents {
        uint32_t exchangeId = 0;
        std::vector<signaling::MediaContent> contents;
    };
    
    struct PendingOutgoingOffer {
        uint32_t exchangeId = 0;
    };
    
    struct PendingOutgoingChannel {
        cricket::MediaDescriptionOptions description;
        
        uint32_t ssrc = 0;
        std::vector<signaling::SsrcGroup> ssrcGroups;
        
        PendingOutgoingChannel(cricket::MediaDescriptionOptions &&description_) :
        description(std::move(description_)) {
        }
    };
    
    struct OutgoingChannel {
        std::string id;
        signaling::MediaContent content;
        
        OutgoingChannel(std::string id_, signaling::MediaContent content_) :
        id(id_), content(content_) {
        }
    };
    
    struct CoordinatedState {
        std::vector<signaling::MediaContent> outgoingContents;
        std::vector<signaling::MediaContent> incomingContents;
    };
    
public:
    ContentCoordinationContext(bool isOutgoing, cricket::ChannelManager *channelManager, rtc::UniqueRandomIdGenerator *uniqueRandomIdGenerator);
    ~ContentCoordinationContext();
    
    void addOutgoingChannel(signaling::MediaContent::Type mediaType);
    
    std::unique_ptr<NegotiationContents> getOffer();
    std::unique_ptr<NegotiationContents> setRemoteNegotiationContent(std::unique_ptr<NegotiationContents> &&remoteNegotiationContent);
    
    std::unique_ptr<CoordinatedState> coordinatedState() const;
    
private:
    std::string takeNextOutgoingChannelId();
    std::unique_ptr<cricket::SessionDescription> currentSessionDescriptionFromCoordinatedState();
    
    std::unique_ptr<NegotiationContents> getAnswer(std::unique_ptr<NegotiationContents> &&offer);
    void setAnswer(std::unique_ptr<NegotiationContents> &&answer);
    
private:
    bool _isOutgoing = false;
    rtc::UniqueRandomIdGenerator *_uniqueRandomIdGenerator = nullptr;
    
    std::unique_ptr<cricket::TransportDescriptionFactory> _transportDescriptionFactory;
    std::unique_ptr<cricket::MediaSessionDescriptionFactory> _sessionDescriptionFactory;
    
    std::vector<std::string> _channelIdOrder;
    
    std::vector<webrtc::RtpHeaderExtensionCapability> _rtpAudioExtensions;
    std::vector<webrtc::RtpHeaderExtensionCapability> _rtpVideoExtensions;
    
    std::vector<PendingOutgoingChannel> _outgoingChannelDescriptions;
    bool _needNegotiation = false;
    
    std::vector<OutgoingChannel> _outgoingChannels;
    std::vector<signaling::MediaContent> _incomingChannels;
    
    std::unique_ptr<PendingOutgoingOffer> _pendingOutgoingOffer;
    
    int _nextOutgoingChannelId = 0;
    
};

} // namespace tgcalls

#endif
