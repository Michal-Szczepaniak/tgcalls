#include "GroupInstanceImpl.h"

#include <memory>
#include "api/scoped_refptr.h"
#include "rtc_base/thread.h"
#include "rtc_base/logging.h"
#include "api/peer_connection_interface.h"
#include "api/task_queue/default_task_queue_factory.h"
#include "media/engine/webrtc_media_engine.h"
#include "api/audio_codecs/audio_decoder_factory_template.h"
#include "api/audio_codecs/audio_encoder_factory_template.h"
#include "api/audio_codecs/opus/audio_decoder_opus.h"
#include "api/audio_codecs/opus/audio_encoder_opus.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/rtc_event_log/rtc_event_log_factory.h"
#include "api/peer_connection_interface.h"
#include "api/video_track_source_proxy.h"
#include "system_wrappers/include/field_trial.h"
#include "api/stats/rtcstats_objects.h"
#include "modules/audio_processing/audio_buffer.h"
#include "common_audio/include/audio_util.h"
#include "common_audio/vad/include/webrtc_vad.h"
#include "modules/audio_processing/agc2/vad_with_level.h"

#include "ThreadLocalObject.h"
#include "Manager.h"
#include "NetworkManager.h"
#include "VideoCaptureInterfaceImpl.h"
#include "platform/PlatformInterface.h"
#include "LogSinkImpl.h"

#include <random>
#include <sstream>
#include <iostream>

namespace tgcalls {

namespace {

static std::vector<std::string> splitSdpLines(std::string const &sdp) {
    std::vector<std::string> result;

    std::istringstream sdpStream(sdp);

    std::string s;
    while (std::getline(sdpStream, s, '\n')) {
        if (s.size() == 0) {
            continue;
        }
        if (s[s.size() - 1] == '\r') {
            s.resize(s.size() - 1);
        }
        result.push_back(s);
    }

    return result;
}

static std::vector<std::string> splitFingerprintLines(std::string const &line) {
    std::vector<std::string> result;

    std::istringstream sdpStream(line);

    std::string s;
    while (std::getline(sdpStream, s, ' ')) {
        if (s.size() == 0) {
            continue;
        }
        result.push_back(s);
    }

    return result;
}

static std::vector<uint32_t> splitSsrcList(std::string const &line) {
    std::vector<uint32_t> result;

    std::istringstream sdpStream(line);

    std::string s;
    while (std::getline(sdpStream, s, ' ')) {
        if (s.size() == 0) {
            continue;
        }
        
        std::istringstream iss(s);
        uint32_t ssrc = 0;
        iss >> ssrc;
        
        result.push_back(ssrc);
    }

    return result;
}

static std::vector<std::string> getLines(std::vector<std::string> const &lines, std::string prefix) {
    std::vector<std::string> result;

    for (auto &line : lines) {
        if (line.find(prefix) == 0) {
            auto cleanLine = line;
            cleanLine.replace(0, prefix.size(), "");
            result.push_back(cleanLine);
        }
    }

    return result;
}

static absl::optional<GroupJoinPayloadVideoPayloadType> parsePayloadType(uint32_t id, std::string const &line) {
    std::string s;
    std::istringstream lineStream(line);
    std::string codec;
    uint32_t clockrate = 0;
    uint32_t channels = 0;
    for (int i = 0; std::getline(lineStream, s, '/'); i++) {
        if (s.size() == 0) {
            continue;
        }
        
        if (i == 0) {
            codec = s;
        } else if (i == 1) {
            std::istringstream iss(s);
            iss >> clockrate;
        } else if (i == 2) {
            std::istringstream iss(s);
            iss >> channels;
        }
    }
    if (codec.size() != 0) {
        GroupJoinPayloadVideoPayloadType payloadType;
        payloadType.id = id;
        payloadType.name = codec;
        payloadType.clockrate = clockrate;
        payloadType.channels = channels;
        return payloadType;
    } else {
        return absl::nullopt;
    }
}

static absl::optional<GroupJoinPayloadVideoPayloadFeedbackType> parseFeedbackType(std::string const &line) {
    std::istringstream lineStream(line);
    std::string s;

    std::string type;
    std::string subtype;
    for (int i = 0; std::getline(lineStream, s, ' '); i++) {
        if (s.size() == 0) {
            continue;
        }
        
        if (i == 0) {
            type = s;
        } else if (i == 1) {
            subtype = s;
        }
    }
    
    if (type.size() != 0) {
        GroupJoinPayloadVideoPayloadFeedbackType parsedType;
        parsedType.type = type;
        parsedType.subtype = subtype;
        return parsedType;
    } else {
        return absl::nullopt;
    }
}

static void parsePayloadParameter(std::string const &line, std::vector<std::pair<std::string, std::string>> &result) {
    std::istringstream lineStream(line);
    std::string s;

    std::string key;
    std::string value;
    for (int i = 0; std::getline(lineStream, s, '='); i++) {
        if (s.size() == 0) {
            continue;
        }
        
        if (i == 0) {
            key = s;
        } else if (i == 1) {
            value = s;
        }
    }
    if (key.size() != 0 && value.size() != 0) {
        result.push_back(std::make_pair(key, value));
    }
}

static std::vector<std::pair<std::string, std::string>> parsePayloadParameters(std::string const &line) {
    std::vector<std::pair<std::string, std::string>> result;
    
    std::istringstream lineStream(line);
    std::string s;

    while (std::getline(lineStream, s, ';')) {
        if (s.size() == 0) {
            continue;
        }
        
        parsePayloadParameter(s, result);
    }
    
    return result;
}

static absl::optional<GroupJoinPayload> parseSdpIntoJoinPayload(std::string const &sdp) {
    GroupJoinPayload result;

    auto lines = splitSdpLines(sdp);

    std::vector<std::string> audioLines;
    std::vector<std::string> videoLines;
    bool isAudioLine = false;
    for (auto &line : lines) {
        if (line.find("m=audio") == 0) {
            isAudioLine = true;
        } else if (line.find("m=video") == 0) {
            isAudioLine = false;
        }
        if (isAudioLine) {
            audioLines.push_back(line);
        } else {
            videoLines.push_back(line);
        }
    }

    result.ssrc = 0;

    auto ufragLines = getLines(audioLines, "a=ice-ufrag:");
    if (ufragLines.size() != 1) {
        return absl::nullopt;
    }
    result.ufrag = ufragLines[0];

    auto pwdLines = getLines(audioLines, "a=ice-pwd:");
    if (pwdLines.size() != 1) {
        return absl::nullopt;
    }
    result.pwd = pwdLines[0];

    for (auto &line : getLines(audioLines, "a=fingerprint:")) {
        auto fingerprintComponents = splitFingerprintLines(line);
        if (fingerprintComponents.size() != 2) {
            continue;
        }

        GroupJoinPayloadFingerprint fingerprint;
        fingerprint.hash = fingerprintComponents[0];
        fingerprint.fingerprint = fingerprintComponents[1];
        fingerprint.setup = "active";
        result.fingerprints.push_back(fingerprint);
    }
    
    for (auto &line : getLines(videoLines, "a=rtpmap:")) {
        std::string s;
        std::istringstream lineStream(line);
        uint32_t id = 0;
        for (int i = 0; std::getline(lineStream, s, ' '); i++) {
            if (s.size() == 0) {
                continue;
            }
            
            if (i == 0) {
                std::istringstream iss(s);
                iss >> id;
            } else if (i == 1) {
                if (id != 0) {
                    auto payloadType = parsePayloadType(id, s);
                    if (payloadType.has_value()) {
                        std::ostringstream fbPrefixStream;
                        fbPrefixStream << "a=rtcp-fb:";
                        fbPrefixStream << id;
                        fbPrefixStream << " ";
                        for (auto &feedbackLine : getLines(videoLines, fbPrefixStream.str())) {
                            auto feedbackType = parseFeedbackType(feedbackLine);
                            if (feedbackType.has_value()) {
                                payloadType->feedbackTypes.push_back(feedbackType.value());
                            }
                        }
                        
                        std::ostringstream parametersPrefixStream;
                        parametersPrefixStream << "a=fmtp:";
                        parametersPrefixStream << id;
                        parametersPrefixStream << " ";
                        for (auto &parametersLine : getLines(videoLines, parametersPrefixStream.str())) {
                            payloadType->parameters = parsePayloadParameters(parametersLine);
                        }
                        
                        result.videoPayloadTypes.push_back(payloadType.value());
                    }
                }
            }
        }
    }
    
    for (auto &line : getLines(videoLines, "a=extmap:")) {
        std::string s;
        std::istringstream lineStream(line);
        uint32_t id = 0;
        for (int i = 0; std::getline(lineStream, s, ' '); i++) {
            if (s.size() == 0) {
                continue;
            }
            
            if (i == 0) {
                std::istringstream iss(s);
                iss >> id;
            } else if (i == 1) {
                if (id != 0) {
                    result.videoExtensionMap.push_back(std::make_pair(id, s));
                }
            }
        }
    }
    
    for (auto &line : getLines(videoLines, "a=ssrc-group:FID ")) {
        auto ssrcs = splitSsrcList(line);
        GroupJoinPayloadVideoSourceGroup group;
        group.semantics = "FID";
        group.ssrcs = ssrcs;
        result.videoSourceGroups.push_back(std::move(group));
    }
    for (auto &line : getLines(videoLines, "a=ssrc-group:SIM ")) {
        auto ssrcs = splitSsrcList(line);
        GroupJoinPayloadVideoSourceGroup group;
        group.semantics = "SIM";
        group.ssrcs = ssrcs;
        result.videoSourceGroups.push_back(std::move(group));
    }

    return result;
}

struct StreamSpec {
    bool isMain = false;
    bool isOutgoing = false;
    uint32_t streamId = 0;
    uint32_t ssrc = 0;
    std::vector<GroupJoinPayloadVideoSourceGroup> videoSourceGroups;
    std::vector<GroupJoinPayloadVideoPayloadType> videoPayloadTypes;
    std::vector<std::pair<uint32_t, std::string>> videoExtensionMap;
    bool isRemoved = false;
};

static void appendSdp(std::vector<std::string> &lines, std::string const &line) {
    lines.push_back(line);
}

static std::string createSdp(uint32_t sessionId, GroupJoinResponsePayload const &payload, bool isAnswer, std::vector<StreamSpec> const &bundleStreams) {
    std::vector<std::string> sdp;

    appendSdp(sdp, "v=0");

    std::ostringstream sessionIdString;
    sessionIdString << "o=- ";
    sessionIdString << sessionId;
    sessionIdString << " 2 IN IP4 0.0.0.0";
    appendSdp(sdp, sessionIdString.str());

    appendSdp(sdp, "s=-");
    appendSdp(sdp, "t=0 0");

    std::ostringstream bundleString;
    bundleString << "a=group:BUNDLE";
    for (auto &stream : bundleStreams) {
        bundleString << " ";
        if (stream.isOutgoing) {
            if (stream.videoPayloadTypes.size() == 0) {
                bundleString << "0";
            } else {
                bundleString << "1";
            }
        } else if (stream.videoPayloadTypes.size() == 0) {
            bundleString << "audio";
            bundleString << stream.streamId;
        } else {
            bundleString << "video";
            bundleString << stream.streamId;
        }
    }
    appendSdp(sdp, bundleString.str());

    appendSdp(sdp, "a=ice-lite");

    for (auto &stream : bundleStreams) {
        std::ostringstream streamMidString;
        if (stream.isOutgoing) {
            if (stream.videoPayloadTypes.size() == 0) {
                streamMidString << "0";
            } else {
                streamMidString << "1";
            }
        } else if (stream.videoPayloadTypes.size() == 0) {
            streamMidString << "audio";
            streamMidString << stream.streamId;
        } else {
            streamMidString << "video";
            streamMidString << stream.streamId;
        }

        std::ostringstream mLineString;
        if (stream.videoPayloadTypes.size() == 0) {
            mLineString << "m=audio ";
        } else {
            mLineString << "m=video ";
        }
        if (stream.isMain) {
            mLineString << "1";
        } else {
            mLineString << "0";
        }
        if (stream.videoPayloadTypes.size() == 0) {
            mLineString << " RTP/SAVPF 111 126";
        } else {
            mLineString << " RTP/SAVPF";
            for (auto &it : stream.videoPayloadTypes) {
                mLineString << " " << it.id;
            }
        }

        appendSdp(sdp, mLineString.str());

        if (stream.isMain) {
            appendSdp(sdp, "c=IN IP4 0.0.0.0");
        }

        std::ostringstream mLineMidString;
        mLineMidString << "a=mid:";
        mLineMidString << streamMidString.str();
        appendSdp(sdp, mLineMidString.str());

        if (stream.isMain) {
            std::ostringstream ufragString;
            ufragString << "a=ice-ufrag:";
            ufragString << payload.ufrag;
            appendSdp(sdp, ufragString.str());

            std::ostringstream pwdString;
            pwdString << "a=ice-pwd:";
            pwdString << payload.pwd;
            appendSdp(sdp, pwdString.str());

            for (auto &fingerprint : payload.fingerprints) {
                std::ostringstream fingerprintString;
                fingerprintString << "a=fingerprint:";
                fingerprintString << fingerprint.hash;
                fingerprintString << " ";
                fingerprintString << fingerprint.fingerprint;
                appendSdp(sdp, fingerprintString.str());
                appendSdp(sdp, "a=setup:passive");
            }

            for (auto &candidate : payload.candidates) {
                std::ostringstream candidateString;
                candidateString << "a=candidate:";
                candidateString << candidate.foundation;
                candidateString << " ";
                candidateString << candidate.component;
                candidateString << " ";
                candidateString << candidate.protocol;
                candidateString << " ";
                candidateString << candidate.priority;
                candidateString << " ";
                candidateString << candidate.ip;
                candidateString << " ";
                candidateString << candidate.port;
                candidateString << " ";
                candidateString << "typ ";
                candidateString << candidate.type;
                candidateString << " ";

                if (candidate.type == "srflx" || candidate.type == "prflx" || candidate.type == "relay") {
                    if (candidate.relAddr.size() != 0 && candidate.relPort.size() != 0) {
                        candidateString << "raddr ";
                        candidateString << candidate.relAddr;
                        candidateString << " ";
                        candidateString << "rport ";
                        candidateString << candidate.relPort;
                        candidateString << " ";
                    }
                }

                if (candidate.protocol == "tcp") {
                    if (candidate.tcpType.size() != 0) {
                        candidateString << "tcptype ";
                        candidateString << candidate.tcpType;
                        candidateString << " ";
                    }
                }

                candidateString << "generation ";
                candidateString << candidate.generation;

                appendSdp(sdp, candidateString.str());
            }
        }

        if (stream.videoPayloadTypes.size() == 0) {
            appendSdp(sdp, "a=rtpmap:111 opus/48000/2");
            appendSdp(sdp, "a=rtpmap:126 telephone-event/8000");
            appendSdp(sdp, "a=fmtp:111 minptime=10; useinbandfec=1");
            appendSdp(sdp, "a=rtcp:1 IN IP4 0.0.0.0");
            appendSdp(sdp, "a=rtcp-mux");
            appendSdp(sdp, "a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level");
            appendSdp(sdp, "a=extmap:3 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time");
            appendSdp(sdp, "a=extmap:5 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01");
            appendSdp(sdp, "a=rtcp-fb:111 transport-cc");
            
            if (isAnswer && stream.isMain) {
                appendSdp(sdp, "a=recvonly");
            } else {
                if (stream.isMain) {
                    appendSdp(sdp, "a=sendrecv");
                } else {
                    appendSdp(sdp, "a=sendonly");
                    appendSdp(sdp, "a=bundle-only");
                }

                if (stream.isRemoved) {
                    appendSdp(sdp, "a=inactive");
                } else {
                    std::ostringstream cnameString;
                    cnameString << "a=ssrc:";
                    cnameString << stream.ssrc;
                    cnameString << " cname:stream";
                    cnameString << stream.streamId;
                    appendSdp(sdp, cnameString.str());

                    std::ostringstream msidString;
                    msidString << "a=ssrc:";
                    msidString << stream.ssrc;
                    msidString << " msid:stream";
                    msidString << stream.streamId;
                    msidString << " audio" << stream.streamId;
                    appendSdp(sdp, msidString.str());

                    std::ostringstream mslabelString;
                    mslabelString << "a=ssrc:";
                    mslabelString << stream.ssrc;
                    mslabelString << " mslabel:audio";
                    mslabelString << stream.streamId;
                    appendSdp(sdp, mslabelString.str());

                    std::ostringstream labelString;
                    labelString << "a=ssrc:";
                    labelString << stream.ssrc;
                    labelString << " label:audio";
                    labelString << stream.streamId;
                    appendSdp(sdp, labelString.str());
                }
            }
        } else {
            appendSdp(sdp, "a=rtcp:1 IN IP4 0.0.0.0");
            appendSdp(sdp, "a=rtcp-mux");
            
            for (auto &it : stream.videoPayloadTypes) {
                std::ostringstream rtpmapString;
                rtpmapString << "a=rtpmap:";
                rtpmapString << it.id;
                rtpmapString << " ";
                rtpmapString << it.name;
                rtpmapString << "/";
                rtpmapString << it.clockrate;
                if (it.channels != 0) {
                    rtpmapString << "/";
                    rtpmapString << it.channels;
                }
                appendSdp(sdp, rtpmapString.str());
                
                for (auto &feedbackType : it.feedbackTypes) {
                    std::ostringstream feedbackString;
                    feedbackString << "a=rtcp-fb:";
                    feedbackString << it.id;
                    feedbackString << " ";
                    feedbackString << feedbackType.type;
                    if (feedbackType.subtype.size() != 0) {
                        feedbackString << " ";
                        feedbackString << feedbackType.subtype;
                    }
                    appendSdp(sdp, feedbackString.str());
                }
                
                if (it.parameters.size() != 0) {
                    std::ostringstream fmtpString;
                    fmtpString << "a=fmtp:";
                    fmtpString << it.id;
                    fmtpString << " ";
                    
                    for (int i = 0; i < it.parameters.size(); i++) {
                        if (i != 0) {
                            fmtpString << ";";
                        }
                        fmtpString << it.parameters[i].first;
                        fmtpString << "=";
                        fmtpString << it.parameters[i].second;
                    }
                    
                    appendSdp(sdp, fmtpString.str());
                }
            }
            
            for (auto &it : stream.videoExtensionMap) {
                std::ostringstream extString;
                extString << "a=extmap:";
                extString << it.first;
                extString << " ";
                extString << it.second;
                appendSdp(sdp, extString.str());
            }
            
            if (isAnswer && stream.isOutgoing) {
                appendSdp(sdp, "a=recvonly");
                appendSdp(sdp, "a=bundle-only");
            } else {
                appendSdp(sdp, "a=sendonly");
                appendSdp(sdp, "a=bundle-only");
                
                if (stream.isRemoved) {
                    appendSdp(sdp, "a=inactive");
                } else {
                    std::vector<uint32_t> ssrcs;
                    for (auto &group : stream.videoSourceGroups) {
                        std::ostringstream groupString;
                        groupString << "a=ssrc-group:";
                        groupString << group.semantics;
                        
                        for (auto ssrc : group.ssrcs) {
                            groupString << " " << ssrc;
                            
                            if (std::find(ssrcs.begin(), ssrcs.end(), ssrc) == ssrcs.end()) {
                                ssrcs.push_back(ssrc);
                            }
                        }
                        
                        appendSdp(sdp, groupString.str());
                    }
                    
                    for (auto ssrc : ssrcs) {
                        std::ostringstream cnameString;
                        cnameString << "a=ssrc:";
                        cnameString << ssrc;
                        cnameString << " cname:stream";
                        cnameString << stream.streamId;
                        appendSdp(sdp, cnameString.str());

                        std::ostringstream msidString;
                        msidString << "a=ssrc:";
                        msidString << ssrc;
                        msidString << " msid:stream";
                        msidString << stream.streamId;
                        msidString << " video" << stream.streamId;
                        appendSdp(sdp, msidString.str());

                        std::ostringstream mslabelString;
                        mslabelString << "a=ssrc:";
                        mslabelString << ssrc;
                        mslabelString << " mslabel:video";
                        mslabelString << stream.streamId;
                        appendSdp(sdp, mslabelString.str());

                        std::ostringstream labelString;
                        labelString << "a=ssrc:";
                        labelString << ssrc;
                        labelString << " label:video";
                        labelString << stream.streamId;
                        appendSdp(sdp, labelString.str());
                    }
                }
            }
        }
    }

    std::ostringstream result;
    for (auto &line : sdp) {
        result << line << "\n";
    }

    return result.str();
}

static std::string parseJoinResponseIntoSdp(uint32_t sessionId, GroupJoinPayload const &joinPayload, GroupJoinResponsePayload const &payload, bool isAnswer, std::vector<GroupParticipantDescription> const &allOtherParticipants) {

    std::vector<StreamSpec> bundleStreams;

    StreamSpec mainStream;
    mainStream.isMain = true;
    mainStream.isOutgoing = true;
    mainStream.streamId = 0;
    mainStream.ssrc = joinPayload.ssrc;
    mainStream.isRemoved = false;
    bundleStreams.push_back(mainStream);
    
    if (joinPayload.videoSourceGroups.size() != 0) {
        StreamSpec mainVideoStream;
        mainVideoStream.isMain = false;
        mainVideoStream.isOutgoing = true;
        mainVideoStream.streamId = joinPayload.videoSourceGroups[0].ssrcs[0];
        mainVideoStream.ssrc = joinPayload.videoSourceGroups[0].ssrcs[0];
        mainVideoStream.videoSourceGroups = joinPayload.videoSourceGroups;
        mainVideoStream.videoPayloadTypes = joinPayload.videoPayloadTypes;
        mainVideoStream.videoExtensionMap = joinPayload.videoExtensionMap;
        
        mainVideoStream.isRemoved = false;
        bundleStreams.push_back(mainVideoStream);
    }

    for (auto &participant : allOtherParticipants) {
        StreamSpec audioStream;
        audioStream.isMain = false;
        
        audioStream.ssrc = participant.audioSsrc;
        audioStream.isRemoved = false;
        audioStream.streamId = participant.audioSsrc;
        bundleStreams.push_back(audioStream);
        
        if (participant.videoPayloadTypes.size() != 0) {
            StreamSpec videoStream;
            videoStream.isMain = false;
            
            videoStream.ssrc = participant.videoSourceGroups[0].ssrcs[0];
            videoStream.isRemoved = false;
            videoStream.streamId = participant.videoSourceGroups[0].ssrcs[0];
            videoStream.videoSourceGroups = participant.videoSourceGroups;
            videoStream.videoExtensionMap = participant.videoExtensionMap;
            videoStream.videoPayloadTypes = participant.videoPayloadTypes;
            
            bundleStreams.push_back(videoStream);
        }
    }

    return createSdp(sessionId, payload, isAnswer, bundleStreams);
}

rtc::Thread *makeNetworkThread() {
    static std::unique_ptr<rtc::Thread> value = rtc::Thread::CreateWithSocketServer();
    value->SetName("WebRTC-Group-Network", nullptr);
    value->Start();
    return value.get();
}

rtc::Thread *getNetworkThread() {
    static rtc::Thread *value = makeNetworkThread();
    return value;
}

rtc::Thread *makeWorkerThread() {
    static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
    value->SetName("WebRTC-Group-Worker", nullptr);
    value->Start();
    return value.get();
}

rtc::Thread *getWorkerThread() {
    static rtc::Thread *value = makeWorkerThread();
    return value;
}

rtc::Thread *getSignalingThread() {
    return Manager::getMediaThread();
}

rtc::Thread *getMediaThread() {
    return Manager::getMediaThread();
}

VideoCaptureInterfaceObject *GetVideoCaptureAssumingSameThread(VideoCaptureInterface *videoCapture) {
    return videoCapture
        ? static_cast<VideoCaptureInterfaceImpl*>(videoCapture)->object()->getSyncAssumingSameThread()
        : nullptr;
}

class PeerConnectionObserverImpl : public webrtc::PeerConnectionObserver {
private:
    std::function<void(std::string, int, std::string)> _discoveredIceCandidate;
    std::function<void(bool)> _connectionStateChanged;
    std::function<void(rtc::scoped_refptr<webrtc::RtpTransceiverInterface>)> _onTrackAdded;
    std::function<void(rtc::scoped_refptr<webrtc::RtpReceiverInterface>)> _onTrackRemoved;
    std::function<void(uint32_t)> _onMissingSsrc;

public:
    PeerConnectionObserverImpl(
        std::function<void(std::string, int, std::string)> discoveredIceCandidate,
        std::function<void(bool)> connectionStateChanged,
        std::function<void(rtc::scoped_refptr<webrtc::RtpTransceiverInterface>)> onTrackAdded,
        std::function<void(rtc::scoped_refptr<webrtc::RtpReceiverInterface>)> onTrackRemoved,
        std::function<void(uint32_t)> onMissingSsrc
    ) :
    _discoveredIceCandidate(discoveredIceCandidate),
    _connectionStateChanged(connectionStateChanged),
    _onTrackAdded(onTrackAdded),
    _onTrackRemoved(onTrackRemoved),
    _onMissingSsrc(onMissingSsrc) {
    }

    virtual void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState new_state) override {
    }

    virtual void OnAddStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {
    }

    virtual void OnRemoveStream(rtc::scoped_refptr<webrtc::MediaStreamInterface> stream) override {
    }

    virtual void OnDataChannel(rtc::scoped_refptr<webrtc::DataChannelInterface> data_channel) override {
    }

    virtual void OnRenegotiationNeeded() override {
    }

    virtual void OnIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
        bool isConnected = false;
        switch (new_state) {
            case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionConnected:
            case webrtc::PeerConnectionInterface::IceConnectionState::kIceConnectionCompleted:
                isConnected = true;
                break;
            default:
                break;
        }
        _connectionStateChanged(isConnected);
    }

    virtual void OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState new_state) override {
    }

    virtual void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState new_state) override {
    }

    virtual void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState new_state) override {
    }

    virtual void OnIceCandidate(const webrtc::IceCandidateInterface* candidate) override {
        std::string sdp;
        candidate->ToString(&sdp);
        _discoveredIceCandidate(sdp, candidate->sdp_mline_index(), candidate->sdp_mid());
    }

    virtual void OnIceCandidateError(const std::string& host_candidate, const std::string& url, int error_code, const std::string& error_text) override {
    }

    virtual void OnIceCandidateError(const std::string& address,
                                     int port,
                                     const std::string& url,
                                     int error_code,
                                     const std::string& error_text) override {
    }

    virtual void OnIceCandidatesRemoved(const std::vector<cricket::Candidate>& candidates) override {
    }

    virtual void OnIceConnectionReceivingChange(bool receiving) override {
    }

    virtual void OnIceSelectedCandidatePairChanged(const cricket::CandidatePairChangeEvent& event) override {
    }

    virtual void OnAddTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver, const std::vector<rtc::scoped_refptr<webrtc::MediaStreamInterface>>& streams) override {
    }

    virtual void OnTrack(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
        /*if (transceiver->receiver()) {
            rtc::scoped_refptr<FrameDecryptorImpl> decryptor(new rtc::RefCountedObject<FrameDecryptorImpl>());
            transceiver->receiver()->SetFrameDecryptor(decryptor);
        }*/

        _onTrackAdded(transceiver);
    }

    virtual void OnRemoveTrack(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override {
        _onTrackRemoved(receiver);
    }

    virtual void OnInterestingUsage(int usage_pattern) override {
    }

    virtual void OnErrorDemuxingPacket(uint32_t ssrc) override {
        _onMissingSsrc(ssrc);
    }
};

class RTCStatsCollectorCallbackImpl : public webrtc::RTCStatsCollectorCallback {
public:
    RTCStatsCollectorCallbackImpl(std::function<void(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &)> completion) :
    _completion(completion) {
    }

    virtual void OnStatsDelivered(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &report) override {
        _completion(report);
    }

private:
    std::function<void(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &)> _completion;
};

static const int kVadResultHistoryLength = 8;

class CombinedVad {
private:
    webrtc::VadWithLevel _vadWithLevel;
    float _vadResultHistory[kVadResultHistoryLength];
    
public:
    CombinedVad() {
        for (int i = 0; i < kVadResultHistoryLength; i++) {
            _vadResultHistory[i] = 0.0f;
        }
    }
    
    ~CombinedVad() {
    }
    
    bool update(webrtc::AudioBuffer *buffer) {
        webrtc::AudioFrameView<float> frameView(buffer->channels(), buffer->num_channels(), buffer->num_frames());
        auto result = _vadWithLevel.AnalyzeFrame(frameView);
        for (int i = 1; i < kVadResultHistoryLength; i++) {
            _vadResultHistory[i - 1] = _vadResultHistory[i];
        }
        _vadResultHistory[kVadResultHistoryLength - 1] = result.speech_probability;
        
        float movingAverage = 0.0f;
        for (int i = 0; i < kVadResultHistoryLength; i++) {
            movingAverage += _vadResultHistory[i];
        }
        movingAverage /= (float)kVadResultHistoryLength;
        
        bool vadResult = false;
        if (movingAverage > 0.8f) {
            vadResult = true;
        }
        
        return vadResult;
    }
};

class AudioTrackSinkInterfaceImpl: public webrtc::AudioTrackSinkInterface {
private:
    std::function<void(float, bool)> _update;
    
    int _peakCount = 0;
    uint16_t _peak = 0;
    
    CombinedVad _vad;

public:
    AudioTrackSinkInterfaceImpl(std::function<void(float, bool)> update) :
    _update(update) {
    }

    virtual ~AudioTrackSinkInterfaceImpl() {
    }

    virtual void OnData(const void *audio_data, int bits_per_sample, int sample_rate, size_t number_of_channels, size_t number_of_frames) override {
        if (bits_per_sample == 16 && number_of_channels == 1) {
            int16_t *samples = (int16_t *)audio_data;
            int numberOfSamplesInFrame = (int)number_of_frames;
            
            webrtc::AudioBuffer buffer(sample_rate, 1, 48000, 1, 48000, 1);
            webrtc::StreamConfig config(sample_rate, 1);
            buffer.CopyFrom(samples, config);
            
            bool vadResult = _vad.update(&buffer);
            
            for (int i = 0; i < numberOfSamplesInFrame; i++) {
                int16_t sample = samples[i];
                if (sample < 0) {
                    sample = -sample;
                }
                if (_peak < sample) {
                    _peak = sample;
                }
                _peakCount += 1;
            }
            
            if (_peakCount >= 1200) {
                float level = ((float)(_peak)) / 4000.0f;
                _peak = 0;
                _peakCount = 0;
                _update(level, vadResult);
            }
        }
    }
};

class CreateSessionDescriptionObserverImpl : public webrtc::CreateSessionDescriptionObserver {
private:
    std::function<void(std::string, std::string)> _completion;

public:
    CreateSessionDescriptionObserverImpl(std::function<void(std::string, std::string)> completion) :
    _completion(completion) {
    }

    virtual void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
        if (desc) {
            std::string sdp;
            desc->ToString(&sdp);

            _completion(sdp, desc->type());
        }
    }

    virtual void OnFailure(webrtc::RTCError error) override {
    }
};

class SetSessionDescriptionObserverImpl : public webrtc::SetSessionDescriptionObserver {
private:
    std::function<void()> _completion;
    std::function<void(webrtc::RTCError)> _error;

public:
    SetSessionDescriptionObserverImpl(std::function<void()> completion, std::function<void(webrtc::RTCError)> error) :
    _completion(completion), _error(error) {
    }

    virtual void OnSuccess() override {
        _completion();
    }

    virtual void OnFailure(webrtc::RTCError error) override {
        _error(error);
    }
};

class AudioCaptureAnalyzer : public webrtc::CustomAudioAnalyzer {
private:
    void Initialize(int sample_rate_hz, int num_channels) override {

    }
    // Analyzes the given capture or render signal.
    void Analyze(const webrtc::AudioBuffer* audio) override {
        _analyze(audio);
    }
    // Returns a string representation of the module state.
    std::string ToString() const override {
        return "analyzing";
    }

    std::function<void(const webrtc::AudioBuffer*)> _analyze;

public:
    AudioCaptureAnalyzer(std::function<void(const webrtc::AudioBuffer*)> analyze) :
    _analyze(analyze) {
    }

    virtual ~AudioCaptureAnalyzer() = default;
};

class WrappedAudioDeviceModule : public webrtc::AudioDeviceModule {
private:
    rtc::scoped_refptr<webrtc::AudioDeviceModule> _impl;
    
public:
    WrappedAudioDeviceModule(rtc::scoped_refptr<webrtc::AudioDeviceModule> impl) :
    _impl(impl) {
    }
    
    virtual ~WrappedAudioDeviceModule() {
    }
    
    virtual int32_t ActiveAudioLayer(AudioLayer *audioLayer) const override {
        return _impl->ActiveAudioLayer(audioLayer);
    }
    
    virtual int32_t RegisterAudioCallback(webrtc::AudioTransport *audioCallback) override {
        return _impl->RegisterAudioCallback(audioCallback);
    }
    
    virtual int32_t Init() override {
        return _impl->Init();
    }
    
    virtual int32_t Terminate() override {
        return _impl->Terminate();
    }
    
    virtual bool Initialized() const override {
        return _impl->Initialized();
    }
    
    virtual int16_t PlayoutDevices() override {
        return _impl->PlayoutDevices();
    }
    
    virtual int16_t RecordingDevices() override {
        return _impl->RecordingDevices();
    }
    
    virtual int32_t PlayoutDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override {
        return _impl->PlayoutDeviceName(index, name, guid);
    }
    
    virtual int32_t RecordingDeviceName(uint16_t index, char name[webrtc::kAdmMaxDeviceNameSize], char guid[webrtc::kAdmMaxGuidSize]) override {
        return _impl->RecordingDeviceName(index, name, guid);
    }
    
    virtual int32_t SetPlayoutDevice(uint16_t index) override {
        return _impl->SetPlayoutDevice(index);
    }
    
    virtual int32_t SetPlayoutDevice(WindowsDeviceType device) override {
        return _impl->SetPlayoutDevice(device);
    }
    
    virtual int32_t SetRecordingDevice(uint16_t index) override {
        return _impl->SetRecordingDevice(index);
    }
    
    virtual int32_t SetRecordingDevice(WindowsDeviceType device) override {
        return _impl->SetRecordingDevice(device);
    }
    
    virtual int32_t PlayoutIsAvailable(bool *available) override {
        return _impl->PlayoutIsAvailable(available);
    }
    
    virtual int32_t InitPlayout() override {
        return _impl->InitPlayout();
    }
    
    virtual bool PlayoutIsInitialized() const override {
        return _impl->PlayoutIsInitialized();
    }
    
    virtual int32_t RecordingIsAvailable(bool *available) override {
        return _impl->RecordingIsAvailable(available);
    }
    
    virtual int32_t InitRecording() override {
        return _impl->InitRecording();
    }
    
    virtual bool RecordingIsInitialized() const override {
        return _impl->RecordingIsInitialized();
    }
    
    virtual int32_t StartPlayout() override {
        return _impl->StartPlayout();
    }
    
    virtual int32_t StopPlayout() override {
        return _impl->StopPlayout();
    }
    
    virtual bool Playing() const override {
        return _impl->Playing();
    }
    
    virtual int32_t StartRecording() override {
        return _impl->StartRecording();
    }
    
    virtual int32_t StopRecording() override {
        return _impl->StopRecording();
    }
    
    virtual bool Recording() const override {
        return _impl->Recording();
    }
    
    virtual int32_t InitSpeaker() override {
        return _impl->InitSpeaker();
    }
    
    virtual bool SpeakerIsInitialized() const override {
        return _impl->SpeakerIsInitialized();
    }
    
    virtual int32_t InitMicrophone() override {
        return _impl->InitMicrophone();
    }
    
    virtual bool MicrophoneIsInitialized() const override {
        return _impl->MicrophoneIsInitialized();
    }
    
    virtual int32_t SpeakerVolumeIsAvailable(bool *available) override {
        return _impl->SpeakerVolumeIsAvailable(available);
    }
    
    virtual int32_t SetSpeakerVolume(uint32_t volume) override {
        return _impl->SetSpeakerVolume(volume);
    }
    
    virtual int32_t SpeakerVolume(uint32_t* volume) const override {
        return _impl->SpeakerVolume(volume);
    }
    
    virtual int32_t MaxSpeakerVolume(uint32_t *maxVolume) const override {
        return _impl->MaxSpeakerVolume(maxVolume);
    }
    
    virtual int32_t MinSpeakerVolume(uint32_t *minVolume) const override {
        return _impl->MinSpeakerVolume(minVolume);
    }
    
    virtual int32_t MicrophoneVolumeIsAvailable(bool *available) override {
        return _impl->MicrophoneVolumeIsAvailable(available);
    }
    
    virtual int32_t SetMicrophoneVolume(uint32_t volume) override {
        return _impl->SetMicrophoneVolume(volume);
    }
    
    virtual int32_t MicrophoneVolume(uint32_t *volume) const override {
        return _impl->MicrophoneVolume(volume);
    }
    
    virtual int32_t MaxMicrophoneVolume(uint32_t *maxVolume) const override {
        return _impl->MaxMicrophoneVolume(maxVolume);
    }
    
    virtual int32_t MinMicrophoneVolume(uint32_t *minVolume) const override {
        return _impl->MinMicrophoneVolume(minVolume);
    }
    
    virtual int32_t SpeakerMuteIsAvailable(bool *available) override {
        return _impl->SpeakerMuteIsAvailable(available);
    }
    
    virtual int32_t SetSpeakerMute(bool enable) override {
        return _impl->SetSpeakerMute(enable);
    }
    
    virtual int32_t SpeakerMute(bool *enabled) const override {
        return _impl->SpeakerMute(enabled);
    }
    
    virtual int32_t MicrophoneMuteIsAvailable(bool *available) override {
        return _impl->MicrophoneMuteIsAvailable(available);
    }
    
    virtual int32_t SetMicrophoneMute(bool enable) override {
        return _impl->SetMicrophoneMute(enable);
    }
    
    virtual int32_t MicrophoneMute(bool *enabled) const override {
        return _impl->MicrophoneMute(enabled);
    }
    
    virtual int32_t StereoPlayoutIsAvailable(bool *available) const override {
        return _impl->StereoPlayoutIsAvailable(available);
    }
    
    virtual int32_t SetStereoPlayout(bool enable) override {
        return _impl->SetStereoPlayout(enable);
    }
    
    virtual int32_t StereoPlayout(bool *enabled) const override {
        return _impl->StereoPlayout(enabled);
    }
    
    virtual int32_t StereoRecordingIsAvailable(bool *available) const override {
        return _impl->StereoRecordingIsAvailable(available);
    }
    
    virtual int32_t SetStereoRecording(bool enable) override {
        return _impl->SetStereoRecording(enable);
    }
    
    virtual int32_t StereoRecording(bool *enabled) const override {
        return _impl->StereoRecording(enabled);
    }
    
    virtual int32_t PlayoutDelay(uint16_t* delayMS) const override {
        return _impl->PlayoutDelay(delayMS);
    }
    
    virtual bool BuiltInAECIsAvailable() const override {
        return _impl->BuiltInAECIsAvailable();
    }
    
    virtual bool BuiltInAGCIsAvailable() const override {
        return _impl->BuiltInAGCIsAvailable();
    }
    
    virtual bool BuiltInNSIsAvailable() const override {
        return _impl->BuiltInNSIsAvailable();
    }
    
    virtual int32_t EnableBuiltInAEC(bool enable) override {
        return _impl->EnableBuiltInAEC(enable);
    }
    
    virtual int32_t EnableBuiltInAGC(bool enable) override {
        return _impl->EnableBuiltInAGC(enable);
    }
    
    virtual int32_t EnableBuiltInNS(bool enable) override {
        return _impl->EnableBuiltInNS(enable);
    }
    
    virtual int32_t GetPlayoutUnderrunCount() const override {
        return _impl->GetPlayoutUnderrunCount();
    }
    
#if defined(WEBRTC_IOS)
    virtual int GetPlayoutAudioParameters(webrtc::AudioParameters *params) const override {
        return _impl->GetPlayoutAudioParameters(params);
    }
    virtual int GetRecordAudioParameters(webrtc::AudioParameters *params) const override {
        return _impl->GetRecordAudioParameters(params);
    }
#endif  // WEBRTC_IOS
};

template <typename Out>
void split(const std::string &s, char delim, Out result) {
    std::istringstream iss(s);
    std::string item;
    while (std::getline(iss, item, delim)) {
        *result++ = item;
    }
}

std::vector<std::string> split(const std::string &s, char delim) {
    std::vector<std::string> elems;
    split(s, delim, std::back_inserter(elems));
    return elems;
}

std::string adjustLocalDescription(const std::string &sdp) {
    std::vector<std::string> lines = split(sdp, '\n');

    std::string pattern = "c=IN ";

    bool foundAudio = false;
    std::stringstream result;
    for (const auto &it : lines) {
        result << it << "\n";
        if (!foundAudio && it.compare(0, pattern.size(), pattern) == 0) {
            foundAudio = true;
            result << "b=AS:" << 32 << "\n";
        }
    }

    return result.str();
}

class CustomVideoSinkInterfaceProxyImpl : public rtc::VideoSinkInterface<webrtc::VideoFrame> {
public:
    CustomVideoSinkInterfaceProxyImpl() {
    }

    virtual ~CustomVideoSinkInterfaceProxyImpl() {
    }

    virtual void OnFrame(const webrtc::VideoFrame& frame) override {
        if (_impl) {
            _impl->OnFrame(frame);
        }
    }

    virtual void OnDiscardedFrame() override {
        if (_impl) {
            _impl->OnDiscardedFrame();
        }
    }

    void setSink(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> impl) {
        _impl = impl;
    }

private:
    std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> _impl;

};

} // namespace



class GroupInstanceManager : public std::enable_shared_from_this<GroupInstanceManager> {
public:
	GroupInstanceManager(GroupInstanceDescriptor &&descriptor) :
    _networkStateUpdated(descriptor.networkStateUpdated),
    _audioLevelsUpdated(descriptor.audioLevelsUpdated),
    _incomingVideoSourcesUpdated(descriptor.incomingVideoSourcesUpdated),
    _initialInputDeviceId(descriptor.initialInputDeviceId),
    _initialOutputDeviceId(descriptor.initialOutputDeviceId),
    _videoCapture(descriptor.videoCapture) {
		auto generator = std::mt19937(std::random_device()());
		auto distribution = std::uniform_int_distribution<uint32_t>();
		do {
            _mainStreamAudioSsrc = distribution(generator);
		} while (!_mainStreamAudioSsrc);
	}

	~GroupInstanceManager() {
        assert(getMediaThread()->IsCurrent());

        destroyAudioDeviceModule();
        if (_peerConnection) {
            _peerConnection->Close();
        }
	}

    void generateAndInsertFakeIncomingSsrc() {
        // At least on Windows recording can't be started without playout.
        // We keep a fake incoming stream, so that playout is always started.
        /*auto generator = std::mt19937(std::random_device()());
        auto distribution = std::uniform_int_distribution<uint32_t>();
        while (true) {
            _fakeIncomingSsrc = distribution(generator);
            if (_fakeIncomingSsrc != 0
                && _fakeIncomingSsrc != _mainStreamAudioSsrc
                && std::find(_allOtherSsrcs.begin(), _allOtherSsrcs.end(), _fakeIncomingSsrc) == _allOtherSsrcs.end()) {
                break;
            }
        }
        _activeOtherSsrcs.emplace(_fakeIncomingSsrc);
        _allOtherSsrcs.emplace_back(_fakeIncomingSsrc);*/
    }

    bool createAudioDeviceModule(
            const webrtc::PeerConnectionFactoryDependencies &dependencies) {
        _adm_thread = dependencies.worker_thread;
        if (!_adm_thread) {
            return false;
        }
        _adm_thread->Invoke<void>(RTC_FROM_HERE, [&] {
            const auto check = [&](webrtc::AudioDeviceModule::AudioLayer layer) {
                auto result = webrtc::AudioDeviceModule::Create(
                    layer,
                    dependencies.task_queue_factory.get());
                return (result && (result->Init() == 0)) ? result : nullptr;
            };
            if (auto result = check(webrtc::AudioDeviceModule::kPlatformDefaultAudio)) {
                _adm_use_withAudioDeviceModule = new rtc::RefCountedObject<WrappedAudioDeviceModule>(result);
#ifdef WEBRTC_LINUX
            } else if (auto result = check(webrtc::AudioDeviceModule::kLinuxAlsaAudio)) {
                _adm_use_withAudioDeviceModule = new rtc::RefCountedObject<WrappedAudioDeviceModule>(result);
#endif // WEBRTC_LINUX
            }
        });
        return (_adm_use_withAudioDeviceModule != nullptr);
    }
    void destroyAudioDeviceModule() {
		if (!_adm_thread) {
			return;
		}
        _adm_thread->Invoke<void>(RTC_FROM_HERE, [&] {
            _adm_use_withAudioDeviceModule = nullptr;
        });
    }

	void start() {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());

        webrtc::field_trial::InitFieldTrialsFromString(
            //"WebRTC-Audio-SendSideBwe/Enabled/"
            "WebRTC-Audio-Allocation/min:6kbps,max:32kbps/"
            "WebRTC-Audio-OpusMinPacketLossRate/Enabled-1/"
            //"WebRTC-FlexFEC-03/Enabled/"
            //"WebRTC-FlexFEC-03-Advertised/Enabled/"
            "WebRTC-PcFactoryDefaultBitrates/min:6kbps,start:32kbps,max:32kbps/"
        );

        PlatformInterface::SharedInstance()->configurePlatformAudio();

        webrtc::PeerConnectionFactoryDependencies dependencies;
        dependencies.network_thread = getNetworkThread();
        dependencies.worker_thread = getWorkerThread();
        dependencies.signaling_thread = getSignalingThread();
        dependencies.task_queue_factory = webrtc::CreateDefaultTaskQueueFactory();

        if (!createAudioDeviceModule(dependencies)) {
            return;
        }

        cricket::MediaEngineDependencies mediaDeps;
        mediaDeps.task_queue_factory = dependencies.task_queue_factory.get();
        mediaDeps.audio_encoder_factory = webrtc::CreateAudioEncoderFactory<webrtc::AudioEncoderOpus>();
        mediaDeps.audio_decoder_factory = webrtc::CreateAudioDecoderFactory<webrtc::AudioDecoderOpus>();
        mediaDeps.video_encoder_factory = PlatformInterface::SharedInstance()->makeVideoEncoderFactory();
        mediaDeps.video_decoder_factory = PlatformInterface::SharedInstance()->makeVideoDecoderFactory();
        mediaDeps.adm = _adm_use_withAudioDeviceModule;
        
        std::shared_ptr<CombinedVad> myVad(new CombinedVad());

        auto analyzer = new AudioCaptureAnalyzer([&, weak, myVad](const webrtc::AudioBuffer* buffer) {
            if (!buffer) {
                return;
            }
            if (buffer->num_channels() != 1) {
                return;
            }

            float peak = 0;
            int peakCount = 0;
            const float *samples = buffer->channels_const()[0];
            for (int i = 0; i < buffer->num_frames(); i++) {
                float sample = samples[i];
                if (sample < 0) {
                    sample = -sample;
                }
                if (peak < sample) {
                    peak = sample;
                }
                peakCount += 1;
            }
            
            bool vadStatus = myVad->update((webrtc::AudioBuffer *)buffer);
            
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, peak, peakCount, vadStatus](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                
                strong->_myAudioLevelPeakCount += peakCount;
                if (strong->_myAudioLevelPeak < peak) {
                    strong->_myAudioLevelPeak = peak;
                }
                if (strong->_myAudioLevelPeakCount >= 1200) {
                    float level = strong->_myAudioLevelPeak / 4000.0f;
                    if (strong->_isMuted) {
                        level = 0.0f;
                    }
                    strong->_myAudioLevelPeak = 0;
                    strong->_myAudioLevelPeakCount = 0;
                    strong->_myAudioLevel = std::make_pair(level, vadStatus);
                }
            });
        });

        webrtc::AudioProcessingBuilder builder;
        builder.SetCaptureAnalyzer(std::unique_ptr<AudioCaptureAnalyzer>(analyzer));
        webrtc::AudioProcessing *apm = builder.Create();
        
        webrtc::AudioProcessing::Config audioConfig;
        webrtc::AudioProcessing::Config::NoiseSuppression noiseSuppression;
        noiseSuppression.enabled = true;
        noiseSuppression.level = webrtc::AudioProcessing::Config::NoiseSuppression::kHigh;
        audioConfig.noise_suppression = noiseSuppression;

        audioConfig.high_pass_filter.enabled = true;

        apm->ApplyConfig(audioConfig);

        mediaDeps.audio_processing = apm;

        mediaDeps.onUnknownAudioSsrc = [weak](uint32_t ssrc) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, ssrc](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->onMissingSsrc(ssrc);
            });
        };

        dependencies.media_engine = cricket::CreateMediaEngine(std::move(mediaDeps));
        dependencies.call_factory = webrtc::CreateCallFactory();
        dependencies.event_log_factory =
            std::make_unique<webrtc::RtcEventLogFactory>(dependencies.task_queue_factory.get());
        dependencies.network_controller_factory = nullptr;
        dependencies.media_transport_factory = nullptr;

        _nativeFactory = webrtc::CreateModularPeerConnectionFactory(std::move(dependencies));

        webrtc::PeerConnectionInterface::RTCConfiguration config;
        config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
        //config.continual_gathering_policy = webrtc::PeerConnectionInterface::ContinualGatheringPolicy::GATHER_CONTINUALLY;
        config.audio_jitter_buffer_fast_accelerate = true;
        config.prioritize_most_likely_ice_candidate_pairs = true;
        config.presume_writable_when_fully_relayed = true;
        //config.audio_jitter_buffer_enable_rtx_handling = true;

        /*webrtc::CryptoOptions cryptoOptions;
        webrtc::CryptoOptions::SFrame sframe;
        sframe.require_frame_encryption = true;
        cryptoOptions.sframe = sframe;
        config.crypto_options = cryptoOptions;*/

        _observer.reset(new PeerConnectionObserverImpl(
            [weak](std::string sdp, int mid, std::string sdpMid) {
                /*getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, mid, sdpMid](){
                    auto strong = weak.lock();
                    if (strong) {
                        //strong->emitIceCandidate(sdp, mid, sdpMid);
                    }
                });*/
            },
            [weak](bool isConnected) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, isConnected](){
                    auto strong = weak.lock();
                    if (strong) {
                        strong->updateIsConnected(isConnected);
                    }
                });
            },
            [weak](rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, transceiver](){
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->onTrackAdded(transceiver);
                });
            },
            [weak](rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, receiver](){
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->onTrackRemoved(receiver);
                });
            },
            [weak](uint32_t ssrc) {
                getMediaThread()->PostTask(RTC_FROM_HERE, [weak, ssrc](){
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->onMissingSsrc(ssrc);
                });
            }
        ));
        _peerConnection = _nativeFactory->CreatePeerConnection(config, nullptr, nullptr, _observer.get());
        assert(_peerConnection != nullptr);

        cricket::AudioOptions options;
        rtc::scoped_refptr<webrtc::AudioSourceInterface> audioSource = _nativeFactory->CreateAudioSource(options);
        std::stringstream name;
        name << "audio";
        name << 0;
        std::vector<std::string> streamIds;
        streamIds.push_back(name.str());
        _localAudioTrack = _nativeFactory->CreateAudioTrack(name.str(), audioSource);
        _localAudioTrack->set_enabled(false);
        auto addedAudioTrack = _peerConnection->AddTrack(_localAudioTrack, streamIds);

        if (addedAudioTrack.ok()) {
            _localAudioTrackSender = addedAudioTrack.value();
            for (auto &it : _peerConnection->GetTransceivers()) {
                if (it->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO) {
                    if (_localAudioTrackSender.get() == it->sender().get()) {
                        it->SetDirection(webrtc::RtpTransceiverDirection::kRecvOnly);
                    }

                    break;
                }
            }
        }
        
        if (_videoCapture) {
            VideoCaptureInterfaceObject *videoCaptureImpl = GetVideoCaptureAssumingSameThread(_videoCapture.get());
            
            _localVideoTrack = _nativeFactory->CreateVideoTrack("video0", videoCaptureImpl->source());
            auto addedVideoTrack = _peerConnection->AddTrack(_localVideoTrack, streamIds);
            if (addedVideoTrack.ok()) {
                for (auto &it : _peerConnection->GetTransceivers()) {
                    if (it->media_type() == cricket::MediaType::MEDIA_TYPE_VIDEO) {
                        if (addedVideoTrack.value().get() == it->sender().get()) {
                            auto capabilities = _nativeFactory->GetRtpSenderCapabilities(
                                cricket::MediaType::MEDIA_TYPE_VIDEO);

                            std::vector<webrtc::RtpCodecCapability> codecs;
                            bool hasH264 = false;
                            for (auto &codec : capabilities.codecs) {
                                if (codec.name == cricket::kVp8CodecName) {
                                    if (!hasH264) {
                                        codecs.insert(codecs.begin(), codec);
                                        hasH264 = true;
                                    }
                                } else if (codec.name == cricket::kRtxCodecName) {
                                    codecs.push_back(codec);
                                }
                            }
                            it->SetCodecPreferences(codecs);

                            break;
                        }
                    }
                }
            }
        }

        setAudioInputDevice(_initialInputDeviceId);
        setAudioOutputDevice(_initialOutputDeviceId);

        // At least on Windows recording doesn't work without started playout.
        withAudioDeviceModule([weak](webrtc::AudioDeviceModule *adm) {
#ifdef WEBRTC_WIN
            // At least on Windows starting/stopping playout while recording
            // is active leads to errors in recording and assertion violation.
			adm->EnableBuiltInAEC(false);
#endif // WEBRTC_WIN

            if (adm->InitPlayout() == 0) {
                adm->StartPlayout();
            } else {
                getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak](){
                    auto strong = weak.lock();
                    if (!strong) {
                        return;
                    }
                    strong->withAudioDeviceModule([](webrtc::AudioDeviceModule *adm) {
                        if (adm->InitPlayout() == 0) {
                            adm->StartPlayout();
                        }
                    });
                }, 2000);
            }
        });

        //beginStatsTimer(100);
        beginLevelsTimer(50);
	}


    void setAudioInputDevice(std::string id) {
#ifndef WEBRTC_IOS
        withAudioDeviceModule([&](webrtc::AudioDeviceModule *adm) {
            const auto recording = adm->Recording();
            if (recording) {
                adm->StopRecording();
            }
            const auto finish = [&] {
                if (recording) {
                    adm->InitRecording();
                    adm->StartRecording();
                }
            };
            if (id == "default" || id.empty()) {
                if (const auto result = adm->SetRecordingDevice(webrtc::AudioDeviceModule::kDefaultCommunicationDevice)) {
                    RTC_LOG(LS_ERROR) << "setAudioInputDevice(" << id << "): SetRecordingDevice(kDefaultCommunicationDevice) failed: " << result << ".";
                } else {
                    RTC_LOG(LS_INFO) << "setAudioInputDevice(" << id << "): SetRecordingDevice(kDefaultCommunicationDevice) success.";
                }
                return finish();
            }
            const auto count = adm
                ? adm->RecordingDevices()
                : int16_t(-666);
            if (count <= 0) {
                RTC_LOG(LS_ERROR) << "setAudioInputDevice(" << id << "): Could not get recording devices count: " << count << ".";
                return finish();
            }
            for (auto i = 0; i != count; ++i) {
                char name[webrtc::kAdmMaxDeviceNameSize + 1] = { 0 };
                char guid[webrtc::kAdmMaxGuidSize + 1] = { 0 };
                adm->RecordingDeviceName(i, name, guid);
                if (id == guid) {
                    const auto result = adm->SetRecordingDevice(i);
                    if (result != 0) {
                        RTC_LOG(LS_ERROR) << "setAudioInputDevice(" << id << ") name '" << std::string(name) << "' failed: " << result << ".";
                    } else {
                        RTC_LOG(LS_INFO) << "setAudioInputDevice(" << id << ") name '" << std::string(name) << "' success.";
                    }
                    return finish();
                }
            }
            RTC_LOG(LS_ERROR) << "setAudioInputDevice(" << id << "): Could not find recording device.";
            return finish();
        });
#endif
    }

    void setAudioOutputDevice(std::string id) {
#ifndef WEBRTC_IOS
        withAudioDeviceModule([&](webrtc::AudioDeviceModule *adm) {
            const auto playing = adm->Playing();
            if (playing) {
                adm->StopPlayout();
            }
            const auto finish = [&] {
                if (playing) {
                    adm->InitPlayout();
                    adm->StartPlayout();
                }
            };
            if (id == "default" || id.empty()) {
                if (const auto result = adm->SetPlayoutDevice(webrtc::AudioDeviceModule::kDefaultCommunicationDevice)) {
                    RTC_LOG(LS_ERROR) << "setAudioOutputDevice(" << id << "): SetPlayoutDevice(kDefaultCommunicationDevice) failed: " << result << ".";
                } else {
                    RTC_LOG(LS_INFO) << "setAudioOutputDevice(" << id << "): SetPlayoutDevice(kDefaultCommunicationDevice) success.";
                }
                return finish();
            }
            const auto count = adm
                ? adm->PlayoutDevices()
                : int16_t(-666);
            if (count <= 0) {
                RTC_LOG(LS_ERROR) << "setAudioOutputDevice(" << id << "): Could not get playout devices count: " << count << ".";
                return finish();
            }
            for (auto i = 0; i != count; ++i) {
                char name[webrtc::kAdmMaxDeviceNameSize + 1] = { 0 };
                char guid[webrtc::kAdmMaxGuidSize + 1] = { 0 };
                adm->PlayoutDeviceName(i, name, guid);
                if (id == guid) {
                    const auto result = adm->SetPlayoutDevice(i);
                    if (result != 0) {
                        RTC_LOG(LS_ERROR) << "setAudioOutputDevice(" << id << ") name '" << std::string(name) << "' failed: " << result << ".";
                    } else {
                        RTC_LOG(LS_INFO) << "setAudioOutputDevice(" << id << ") name '" << std::string(name) << "' success.";
                    }
                    return finish();
                }
            }
            RTC_LOG(LS_ERROR) << "setAudioOutputDevice(" << id << "): Could not find playout device.";
            return finish();
        });
#endif
    }
    
    void setIncomingVideoOutput(uint32_t ssrc, std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
        auto current = _remoteVideoTrackSinks.find(ssrc);
        if (current != _remoteVideoTrackSinks.end()) {
            current->second->setSink(sink);
        } else {
            std::unique_ptr<CustomVideoSinkInterfaceProxyImpl> sinkProxy(new CustomVideoSinkInterfaceProxyImpl());
            sinkProxy->setSink(sink);
            _remoteVideoTrackSinks[ssrc] = std::move(sinkProxy);
        }
    }

    void updateIsConnected(bool isConnected) {
        _isConnected = isConnected;

        auto timestamp = rtc::TimeMillis();

        _isConnectedUpdateValidTaskId++;

        if (!isConnected && _appliedOfferTimestamp > timestamp - 1000) {
            auto taskId = _isConnectedUpdateValidTaskId;
            const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
            getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak, taskId]() {
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                if (strong->_isConnectedUpdateValidTaskId == taskId) {
                    strong->_networkStateUpdated(strong->_isConnected);
                }
            }, 1000);
        } else {
            _networkStateUpdated(_isConnected);
        }
    }

    void stop() {
        _peerConnection->Close();
    }

    void emitJoinPayload(std::function<void(GroupJoinPayload)> completion) {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>([weak, completion](std::string sdp, std::string type) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, type, completion](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }

                auto lines = splitSdpLines(sdp);
                std::vector<std::string> resultSdp;

                std::ostringstream generatedSsrcStringStream;
                generatedSsrcStringStream << strong->_mainStreamAudioSsrc;
                auto generatedSsrcString = generatedSsrcStringStream.str();

                bool isAudio = false;
                for (auto &line : lines) {
                    auto adjustedLine = line;
                    
                    if (adjustedLine.find("m=audio") == 0) {
                        isAudio = true;
                    } else if (adjustedLine.find("m=video") == 0) {
                        isAudio = false;
                    }
                    
                    if (isAudio) {
                        if (adjustedLine.find("a=ssrc:") == 0) {
                            int startIndex = 7;
                            int i = startIndex;
                            while (i < adjustedLine.size()) {
                                if (!isdigit(adjustedLine[i])) {
                                    break;
                                }
                                i++;
                            }
                            if (i >= startIndex) {
                                adjustedLine.replace(startIndex, i - startIndex, generatedSsrcString);
                            }
                        }
                    }
                    
                    appendSdp(resultSdp, adjustedLine);
                }

                std::ostringstream result;
                for (auto &line : resultSdp) {
                    result << line << "\n";
                }

                auto adjustedSdp = result.str();

                RTC_LOG(LoggingSeverity::WARNING) << "----- setLocalDescription join -----";
                RTC_LOG(LoggingSeverity::WARNING) << adjustedSdp;
                RTC_LOG(LoggingSeverity::WARNING) << "-----";

                webrtc::SdpParseError error;
                webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, adjustLocalDescription(adjustedSdp), &error);
                if (sessionDescription != nullptr) {
                    rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, adjustedSdp, completion]() {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        auto payload = parseSdpIntoJoinPayload(adjustedSdp);
                        if (payload) {
                            payload->ssrc = strong->_mainStreamAudioSsrc;
                            strong->_joinPayload = payload;
                            completion(payload.value());
                        }
                    }, [](webrtc::RTCError error) {
                    }));
                    strong->_peerConnection->SetLocalDescription(observer, sessionDescription);
                } else {
                    return;
                }
            });
        }));
        _peerConnection->CreateOffer(observer, options);
    }

    void setJoinResponsePayload(GroupJoinResponsePayload payload, std::vector<tgcalls::GroupParticipantDescription> &&participants) {
        if (!_joinPayload) {
            return;
        }
        _joinResponsePayload = payload;
        auto sdp = parseJoinResponseIntoSdp(_sessionId, _joinPayload.value(), payload, true, _allOtherParticipants);
        setOfferSdp(sdp, true, true, false);
        
        addParticipantsInternal(std::move(participants), false);
    }

    void removeSsrcs(std::vector<uint32_t> ssrcs) {
        /*if (!_joinPayload) {
            return;
        }
        if (!_joinResponsePayload) {
            return;
        }

        bool updated = false;
        for (auto ssrc : ssrcs) {
            if (std::find(_allOtherSsrcs.begin(), _allOtherSsrcs.end(), ssrc) != _allOtherSsrcs.end() && std::find(_activeOtherSsrcs.begin(), _activeOtherSsrcs.end(), ssrc) != _activeOtherSsrcs.end()) {
                if (!_fakeIncomingSsrc || ssrc == _fakeIncomingSsrc) {
                    generateAndInsertFakeIncomingSsrc();
                }
                _activeOtherSsrcs.erase(ssrc);
                updated = true;
            }
        }

        if (updated) {
            auto sdp = parseJoinResponseIntoSdp(_sessionId, _joinPayload.value(), _joinResponsePayload.value(), false, _allOtherSsrcs, _activeOtherSsrcs);
            setOfferSdp(sdp, false, false, false);
        }*/
    }
    
    void addParticipants(std::vector<GroupParticipantDescription> &&participants) {
        addParticipantsInternal(std::move(participants), false);
    }

    void addParticipantsInternal(std::vector<GroupParticipantDescription> const &participants, bool completeMissingSsrcSetup) {
        if (!_joinPayload || !_joinResponsePayload) {
            if (completeMissingSsrcSetup) {
                completeProcessingMissingSsrcs();
            }
            return;
        }

        for (auto &participant : participants) {
            bool found = false;
            for (auto &other : _allOtherParticipants) {
                if (other.audioSsrc == participant.audioSsrc) {
                    found = true;
                    break;
                }
            }
            
            if (!found) {
                _allOtherParticipants.push_back(participant);
                //_activeOtherSsrcs.insert(participant.audioSsrc);
            }
        }

        auto sdp = parseJoinResponseIntoSdp(_sessionId, _joinPayload.value(), _joinResponsePayload.value(), false, _allOtherParticipants);
        setOfferSdp(sdp, false, false, completeMissingSsrcSetup);
    }

    void applyLocalSdp() {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>([weak](std::string sdp, std::string type) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, type](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }

                auto lines = splitSdpLines(sdp);
                std::vector<std::string> resultSdp;

                std::ostringstream generatedSsrcStringStream;
                generatedSsrcStringStream << strong->_mainStreamAudioSsrc;
                auto generatedSsrcString = generatedSsrcStringStream.str();

                bool isAudio = false;
                
                for (auto &line : lines) {
                    auto adjustedLine = line;
                    
                    if (adjustedLine.find("m=audio") == 0) {
                        isAudio = true;
                    } else if (adjustedLine.find("m=video") == 0) {
                        isAudio = false;
                    }
                    
                    if (isAudio) {
                        if (adjustedLine.find("a=ssrc:") == 0) {
                            int startIndex = 7;
                            int i = startIndex;
                            while (i < adjustedLine.size()) {
                                if (!isdigit(adjustedLine[i])) {
                                    break;
                                }
                                i++;
                            }
                            if (i >= startIndex) {
                                adjustedLine.replace(startIndex, i - startIndex, generatedSsrcString);
                            }
                        }
                    }
                    
                    appendSdp(resultSdp, adjustedLine);
                }

                std::ostringstream result;
                for (auto &line : resultSdp) {
                    result << line << "\n";
                }

                auto adjustedSdp = result.str();

                RTC_LOG(LoggingSeverity::WARNING) << "----- setLocalDescription applyLocalSdp -----";
                RTC_LOG(LoggingSeverity::WARNING) << adjustedSdp;
                RTC_LOG(LoggingSeverity::WARNING) << "-----";

                webrtc::SdpParseError error;
                webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, adjustLocalDescription(adjustedSdp), &error);
                if (sessionDescription != nullptr) {
                    rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, adjustedSdp]() {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }

                        if (!strong->_joinPayload) {
                            return;
                        }
                        if (!strong->_joinResponsePayload) {
                            return;
                        }

                        auto sdp = parseJoinResponseIntoSdp(strong->_sessionId, strong->_joinPayload.value(), strong->_joinResponsePayload.value(), true, strong->_allOtherParticipants);
                        strong->setOfferSdp(sdp, false, true, false);
                    }, [](webrtc::RTCError error) {
                    }));
                    strong->_peerConnection->SetLocalDescription(observer, sessionDescription);
                } else {
                    return;
                }
            });
        }));
        _peerConnection->CreateOffer(observer, options);
    }

    void setOfferSdp(std::string const &offerSdp, bool isInitialJoinAnswer, bool isAnswer, bool completeMissingSsrcSetup) {
        if (!isAnswer && _appliedRemoteRescription == offerSdp) {
            if (completeMissingSsrcSetup) {
                completeProcessingMissingSsrcs();
            }
            return;
        }
        _appliedRemoteRescription = offerSdp;

        RTC_LOG(LoggingSeverity::WARNING) << "----- setOfferSdp " << (isAnswer ? "answer" : "offer") << " -----";
        RTC_LOG(LoggingSeverity::WARNING) << offerSdp;
        RTC_LOG(LoggingSeverity::WARNING) << "-----";

        webrtc::SdpParseError error;
        webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(isAnswer ? "answer" : "offer", adjustLocalDescription(offerSdp), &error);
        if (!sessionDescription) {
            if (completeMissingSsrcSetup) {
                completeProcessingMissingSsrcs();
            }
            return;
        }

        if (!isAnswer) {
            _appliedOfferTimestamp = rtc::TimeMillis();
        }

        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, isInitialJoinAnswer, isAnswer, completeMissingSsrcSetup]() {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, isInitialJoinAnswer, isAnswer, completeMissingSsrcSetup](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                if (!isAnswer) {
                    strong->emitAnswer(completeMissingSsrcSetup);
                } else {
                    if (isInitialJoinAnswer) {
                        strong->completedInitialSetup();
                    }
                    
                    if (completeMissingSsrcSetup) {
                        strong->completeProcessingMissingSsrcs();
                    }
                }
            });
        }, [weak, completeMissingSsrcSetup](webrtc::RTCError error) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, completeMissingSsrcSetup](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                if (completeMissingSsrcSetup) {
                    strong->completeProcessingMissingSsrcs();
                }
            });
        }));

        _peerConnection->SetRemoteDescription(observer, sessionDescription);
    }

    void beginStatsTimer(int timeoutMs) {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->collectStats();
            });
        }, timeoutMs);
    }

    void beginLevelsTimer(int timeoutMs) {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            auto strong = weak.lock();
            if (!strong) {
                return;
            }

            std::vector<std::pair<uint32_t, std::pair<float, bool>>> levels;
            for (auto &it : strong->_audioLevels) {
                if (it.second.first > 0.001f) {
                    levels.push_back(std::make_pair(it.first, it.second));
                }
            }
            levels.push_back(std::make_pair(0, strong->_myAudioLevel));
            
            strong->_audioLevels.clear();
            strong->_audioLevelsUpdated(levels);
            
            strong->beginLevelsTimer(50);
        }, timeoutMs);
    }

    void collectStats() {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());

        rtc::scoped_refptr<RTCStatsCollectorCallbackImpl> observer(new rtc::RefCountedObject<RTCStatsCollectorCallbackImpl>([weak](const rtc::scoped_refptr<const webrtc::RTCStatsReport> &stats) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, stats](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->reportStats(stats);
                strong->beginStatsTimer(100);
            });
        }));
        _peerConnection->GetStats(observer);
    }

    void reportStats(const rtc::scoped_refptr<const webrtc::RTCStatsReport> &stats) {
    }

    void onTrackAdded(rtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
        if (transceiver->direction() == webrtc::RtpTransceiverDirection::kRecvOnly && transceiver->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO) {
            if (transceiver->mid()) {
                auto streamId = transceiver->mid().value();
                if (streamId.find("audio") != 0) {
                    return;
                }
                streamId.replace(0, 5, "");
                std::istringstream iss(streamId);
                uint32_t ssrc = 0;
                iss >> ssrc;

                auto remoteAudioTrack = static_cast<webrtc::AudioTrackInterface *>(transceiver->receiver()->track().get());
                if (_audioTrackSinks.find(ssrc) == _audioTrackSinks.end()) {
                    const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
                    std::shared_ptr<AudioTrackSinkInterfaceImpl> sink(new AudioTrackSinkInterfaceImpl([weak, ssrc](float level, bool hasSpeech) {
                        getMediaThread()->PostTask(RTC_FROM_HERE, [weak, ssrc, level, hasSpeech]() {
                            auto strong = weak.lock();
                            if (!strong) {
                                return;
                            }
                            auto current = strong->_audioLevels.find(ssrc);
                            if (current != strong->_audioLevels.end()) {
                                if (current->second.first < level) {
                                    strong->_audioLevels[ssrc] = std::make_pair(level, hasSpeech);
                                }
                            } else {
                                strong->_audioLevels[ssrc] = std::make_pair(level, hasSpeech);
                            }
                        });
                    }));
                    _audioTrackSinks[ssrc] = sink;
                    remoteAudioTrack->AddSink(sink.get());
                }
            }
        } else if (transceiver->direction() == webrtc::RtpTransceiverDirection::kRecvOnly && transceiver->media_type() == cricket::MediaType::MEDIA_TYPE_VIDEO) {
            auto streamId = transceiver->mid().value();
            if (streamId.find("video") != 0) {
                return;
            }
            streamId.replace(0, 5, "");
            std::istringstream iss(streamId);
            uint32_t ssrc = 0;
            iss >> ssrc;
            
            auto remoteVideoTrack = static_cast<webrtc::VideoTrackInterface *>(transceiver->receiver()->track().get());
            if (_remoteVideoTracks.find(ssrc) == _remoteVideoTracks.end()) {
                _remoteVideoTracks[ssrc] = remoteVideoTrack;
                auto current = _remoteVideoTrackSinks.find(ssrc);
                if (current != _remoteVideoTrackSinks.end()) {
                    remoteVideoTrack->AddOrUpdateSink(current->second.get(), rtc::VideoSinkWants());
                } else {
                    std::unique_ptr<CustomVideoSinkInterfaceProxyImpl> sink(new CustomVideoSinkInterfaceProxyImpl());
                    remoteVideoTrack->AddOrUpdateSink(sink.get(), rtc::VideoSinkWants());
                    _remoteVideoTrackSinks[ssrc] = std::move(sink);
                }
                
                if (_incomingVideoSourcesUpdated) {
                    std::vector<uint32_t> allSources;
                    for (auto &it : _remoteVideoTracks) {
                        allSources.push_back(it.first);
                    }
                    _incomingVideoSourcesUpdated(allSources);
                }
            }
        }
    }

    void onTrackRemoved(rtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) {
    }

    void onMissingSsrc(uint32_t ssrc) {
        /*if (_processedMissingSsrcs.find(ssrc) == _processedMissingSsrcs.end()) {
            _processedMissingSsrcs.insert(ssrc);

            _missingSsrcQueue.insert(ssrc);
            if (!_isProcessingMissingSsrcs) {
                beginProcessingMissingSsrcs();
            }
        }*/
    }

    void beginProcessingMissingSsrcs() {
        if (_isProcessingMissingSsrcs) {
            return;
        }
        _isProcessingMissingSsrcs = true;
        auto timestamp = rtc::TimeMillis();
        if (timestamp > _missingSsrcsProcessedTimestamp + 200) {
            applyMissingSsrcs();
        } else {
            const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
            getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                strong->applyMissingSsrcs();
            }, 200);
        }
    }
    
    void applyMissingSsrcs() {
        assert(_isProcessingMissingSsrcs);
        if (_missingSsrcQueue.size() == 0) {
            completeProcessingMissingSsrcs();
            return;
        }
        
        std::vector<GroupParticipantDescription> addParticipants;
        for (auto ssrc : _missingSsrcQueue) {
            GroupParticipantDescription participant;
            participant.audioSsrc = ssrc;
            addParticipants.push_back(participant);
        }
        _missingSsrcQueue.clear();
        
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        
        addParticipantsInternal(addParticipants, true);
    }
    
    void completeProcessingMissingSsrcs() {
        assert(_isProcessingMissingSsrcs);
        _isProcessingMissingSsrcs = false;
        _missingSsrcsProcessedTimestamp = rtc::TimeMillis();
        
        if (_missingSsrcQueue.size() != 0) {
            beginProcessingMissingSsrcs();
        }
    }
    
    void completedInitialSetup() {
        //beginDebugSsrcTimer(1000);
    }
    
    uint32_t _nextTestSsrc = 100;
    
    void beginDebugSsrcTimer(int timeout) {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());
        getMediaThread()->PostDelayedTask(RTC_FROM_HERE, [weak]() {
            auto strong = weak.lock();
            if (!strong) {
                return;
            }
            
            if (strong->_nextTestSsrc >= 100 + 50) {
                return;
            }

            strong->_nextTestSsrc++;
            strong->onMissingSsrc(strong->_nextTestSsrc);

            strong->beginDebugSsrcTimer(20);
        }, timeout);
    }
    
    void setIsMuted(bool isMuted) {
        if (!_localAudioTrackSender) {
            return;
        }
        if (_isMuted == isMuted) {
            return;
        }

        for (auto &it : _peerConnection->GetTransceivers()) {
            if (it->media_type() == cricket::MediaType::MEDIA_TYPE_AUDIO) {
                if (_localAudioTrackSender.get() == it->sender().get()) {
                    if (isMuted) {
                        /*if (it->direction() == webrtc::RtpTransceiverDirection::kSendRecv) {
                            it->SetDirection(webrtc::RtpTransceiverDirection::kRecvOnly);

                            applyLocalSdp();

                            break;
                        }*/
                    } else {
                        if (it->direction() == webrtc::RtpTransceiverDirection::kRecvOnly) {
                            it->SetDirection(webrtc::RtpTransceiverDirection::kSendRecv);

                            applyLocalSdp();

                            break;
                        }
                    }
                }

                break;
            }
        }

        _isMuted = isMuted;
        _localAudioTrack->set_enabled(!isMuted);
        
        RTC_LOG(LoggingSeverity::WARNING) << "setIsMuted: " << isMuted;
    }

    void emitAnswer(bool completeMissingSsrcSetup) {
        const auto weak = std::weak_ptr<GroupInstanceManager>(shared_from_this());

        webrtc::PeerConnectionInterface::RTCOfferAnswerOptions options;
        rtc::scoped_refptr<CreateSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<CreateSessionDescriptionObserverImpl>([weak, completeMissingSsrcSetup](std::string sdp, std::string type) {
            getMediaThread()->PostTask(RTC_FROM_HERE, [weak, sdp, type, completeMissingSsrcSetup](){
                auto strong = weak.lock();
                if (!strong) {
                    return;
                }
                
                RTC_LOG(LoggingSeverity::WARNING) << "----- setLocalDescription answer -----";
                RTC_LOG(LoggingSeverity::WARNING) << sdp;
                RTC_LOG(LoggingSeverity::WARNING) << "-----";

                webrtc::SdpParseError error;
                webrtc::SessionDescriptionInterface *sessionDescription = webrtc::CreateSessionDescription(type, adjustLocalDescription(sdp), &error);
                if (sessionDescription != nullptr) {
                    rtc::scoped_refptr<SetSessionDescriptionObserverImpl> observer(new rtc::RefCountedObject<SetSessionDescriptionObserverImpl>([weak, sdp, completeMissingSsrcSetup]() {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        
                        if (completeMissingSsrcSetup) {
                            strong->completeProcessingMissingSsrcs();
                        }
                    }, [weak, completeMissingSsrcSetup](webrtc::RTCError error) {
                        auto strong = weak.lock();
                        if (!strong) {
                            return;
                        }
                        
                        if (completeMissingSsrcSetup) {
                            strong->completeProcessingMissingSsrcs();
                        }
                    }));
                    strong->_peerConnection->SetLocalDescription(observer, sessionDescription);
                } else {
                    if (completeMissingSsrcSetup) {
                        strong->completeProcessingMissingSsrcs();
                    }
                }
            });
        }));
        _peerConnection->CreateAnswer(observer, options);
    }

private:
    void withAudioDeviceModule(std::function<void(webrtc::AudioDeviceModule*)> callback) {
        _adm_thread->Invoke<void>(RTC_FROM_HERE, [&] {
            callback(_adm_use_withAudioDeviceModule.get());
        });
    }

    std::function<void(bool)> _networkStateUpdated;
    std::function<void(std::vector<std::pair<uint32_t, std::pair<float, bool>>> const &)> _audioLevelsUpdated;
    std::function<void(std::vector<uint32_t> const &)> _incomingVideoSourcesUpdated;
    
    int32_t _myAudioLevelPeakCount = 0;
    float _myAudioLevelPeak = 0;
    std::pair<float, bool> _myAudioLevel;

    std::string _initialInputDeviceId;
    std::string _initialOutputDeviceId;

    uint32_t _sessionId = 6543245;
    uint32_t _mainStreamAudioSsrc = 0;
    absl::optional<GroupJoinPayload> _joinPayload;
    uint32_t _fakeIncomingSsrc = 0;
    absl::optional<GroupJoinResponsePayload> _joinResponsePayload;

    int64_t _appliedOfferTimestamp = 0;
    bool _isConnected = false;
    int _isConnectedUpdateValidTaskId = 0;
    
    bool _isMuted = true;

    std::vector<GroupParticipantDescription> _allOtherParticipants;
    std::set<uint32_t> _processedMissingSsrcs;
    
    int64_t _missingSsrcsProcessedTimestamp = 0;
    bool _isProcessingMissingSsrcs = false;
    std::set<uint32_t> _missingSsrcQueue;

    std::string _appliedRemoteRescription;

    rtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> _nativeFactory;
    std::unique_ptr<PeerConnectionObserverImpl> _observer;
    rtc::scoped_refptr<webrtc::PeerConnectionInterface> _peerConnection;
    std::unique_ptr<AudioTrackSinkInterfaceImpl> _localAudioTrackSink;
    rtc::scoped_refptr<webrtc::AudioTrackInterface> _localAudioTrack;
    rtc::scoped_refptr<webrtc::RtpSenderInterface> _localAudioTrackSender;
    
    rtc::scoped_refptr<webrtc::VideoTrackInterface> _localVideoTrack;

    rtc::Thread *_adm_thread = nullptr;
    rtc::scoped_refptr<webrtc::AudioDeviceModule> _adm_use_withAudioDeviceModule;

    std::map<uint32_t, std::shared_ptr<AudioTrackSinkInterfaceImpl>> _audioTrackSinks;
    std::map<uint32_t, std::pair<float, bool>> _audioLevels;
    
    std::map<uint32_t, rtc::scoped_refptr<webrtc::VideoTrackInterface>> _remoteVideoTracks;
    std::map<uint32_t, std::unique_ptr<CustomVideoSinkInterfaceProxyImpl>> _remoteVideoTrackSinks;
    
    std::shared_ptr<VideoCaptureInterface> _videoCapture;
};

GroupInstanceImpl::GroupInstanceImpl(GroupInstanceDescriptor &&descriptor)
: _logSink(std::make_unique<LogSinkImpl>(descriptor.config.logPath)) {
    rtc::LogMessage::LogToDebug(rtc::LS_INFO);
    rtc::LogMessage::SetLogToStderr(true);
    if (_logSink) {
		rtc::LogMessage::AddLogToStream(_logSink.get(), rtc::LS_INFO);
	}

	_manager.reset(new ThreadLocalObject<GroupInstanceManager>(getMediaThread(), [descriptor = std::move(descriptor)]() mutable {
		return new GroupInstanceManager(std::move(descriptor));
	}));
	_manager->perform(RTC_FROM_HERE, [](GroupInstanceManager *manager) {
		manager->start();
	});
}

GroupInstanceImpl::~GroupInstanceImpl() {
	if (_logSink) {
		rtc::LogMessage::RemoveLogToStream(_logSink.get());
	}
    _manager = nullptr;

    // Wait until _manager is destroyed, otherwise there is a race condition
    // in destruction of PeerConnection on media thread and network thread.
    getMediaThread()->Invoke<void>(RTC_FROM_HERE, [] {});
}

void GroupInstanceImpl::stop() {
    _manager->perform(RTC_FROM_HERE, [](GroupInstanceManager *manager) {
        manager->stop();
    });
}

void GroupInstanceImpl::emitJoinPayload(std::function<void(GroupJoinPayload)> completion) {
    _manager->perform(RTC_FROM_HERE, [completion](GroupInstanceManager *manager) {
        manager->emitJoinPayload(completion);
    });
}

void GroupInstanceImpl::setJoinResponsePayload(GroupJoinResponsePayload payload, std::vector<tgcalls::GroupParticipantDescription> &&participants) {
    _manager->perform(RTC_FROM_HERE, [payload, participants = std::move(participants)](GroupInstanceManager *manager) mutable {
        manager->setJoinResponsePayload(payload, std::move(participants));
    });
}

void GroupInstanceImpl::removeSsrcs(std::vector<uint32_t> ssrcs) {
    _manager->perform(RTC_FROM_HERE, [ssrcs](GroupInstanceManager *manager) {
        manager->removeSsrcs(ssrcs);
    });
}

void GroupInstanceImpl::addParticipants(std::vector<GroupParticipantDescription> &&participants) {
    _manager->perform(RTC_FROM_HERE, [participants = std::move(participants)](GroupInstanceManager *manager) mutable {
        manager->addParticipants(std::move(participants));
    });
}

void GroupInstanceImpl::setIsMuted(bool isMuted) {
    _manager->perform(RTC_FROM_HERE, [isMuted](GroupInstanceManager *manager) {
        manager->setIsMuted(isMuted);
    });
}

void GroupInstanceImpl::setAudioInputDevice(std::string id) {
    _manager->perform(RTC_FROM_HERE, [id](GroupInstanceManager *manager) {
        manager->setAudioInputDevice(id);
    });
}
void GroupInstanceImpl::setAudioOutputDevice(std::string id) {
    _manager->perform(RTC_FROM_HERE, [id](GroupInstanceManager *manager) {
        manager->setAudioOutputDevice(id);
    });
}

void GroupInstanceImpl::setIncomingVideoOutput(uint32_t ssrc, std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
    _manager->perform(RTC_FROM_HERE, [ssrc, sink](GroupInstanceManager *manager) {
        manager->setIncomingVideoOutput(ssrc, sink);
    });
}

} // namespace tgcalls
