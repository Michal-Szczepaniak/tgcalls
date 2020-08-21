#include "Manager.h"

#include "rtc_base/byte_buffer.h"

#include <fstream>

namespace tgcalls {
namespace {

rtc::Thread *makeNetworkThread() {
	static std::unique_ptr<rtc::Thread> value = rtc::Thread::CreateWithSocketServer();
	value->SetName("WebRTC-Network", nullptr);
	value->Start();
	return value.get();
}

rtc::Thread *getNetworkThread() {
	static rtc::Thread *value = makeNetworkThread();
	return value;
}

rtc::Thread *makeMediaThread() {
	static std::unique_ptr<rtc::Thread> value = rtc::Thread::Create();
	value->SetName("WebRTC-Media", nullptr);
	value->Start();
	return value.get();
}

void dumpStatsLog(const std::string path, const CallStats &stats) {
    std::ofstream file;
    file.open(path);
    
    file << "{";
    file << "\"v\":\"" << 1 << "\"";
    file << ",";
    
    file << "\"codec\":\"" << stats.outgoingCodec << "\"";
    file << ",";
    
    file << "\"bitrate\":[";
    bool addComma = false;
    for (auto &it : stats.bitrateRecords) {
        if (addComma) {
            file << ",";
        }
        file << "{";
        file << "\"t\":\"" << it.timestamp << "\"";
        file << ",";
        file << "\"b\":\"" << it.bitrate << "\"";
        file << "}";
        addComma = true;
    }
    file << "]";
    file << ",";
    
    file << "\"network\":[";
    addComma = false;
    for (auto &it : stats.networkRecords) {
        if (addComma) {
            file << ",";
        }
        file << "{";
        file << "\"t\":\"" << it.timestamp << "\"";
        file << ",";
        file << "\"e\":\"" << (int)(it.endpointType) << "\"";
        file << ",";
        file << "\"w\":\"" << (it.isLowCost ? 1 : 0) << "\"";
        file << "}";
        addComma = true;
    }
    file << "]";
    
    file << "}";
    
    file.close();
}

} // namespace

rtc::Thread *Manager::getMediaThread() {
	static rtc::Thread *value = makeMediaThread();
	return value;
}

Manager::Manager(rtc::Thread *thread, Descriptor &&descriptor) :
_thread(thread),
_encryptionKey(descriptor.encryptionKey),
_signaling(
	EncryptedConnection::Type::Signaling,
	_encryptionKey,
	[=](int delayMs, int cause) { sendSignalingAsync(delayMs, cause); }),
_enableP2P(descriptor.config.enableP2P),
_enableTCP(descriptor.config.allowTCP),
_enableStunMarking(descriptor.config.enableStunMarking),
_protocolVersion(descriptor.config.protocolVersion),
_statsLogPath(descriptor.config.statsLogPath),
_rtcServers(std::move(descriptor.rtcServers)),
_videoCapture(std::move(descriptor.videoCapture)),
_stateUpdated(std::move(descriptor.stateUpdated)),
_remoteMediaStateUpdated(std::move(descriptor.remoteMediaStateUpdated)),
_remoteBatteryLevelIsLowUpdated(std::move(descriptor.remoteBatteryLevelIsLowUpdated)),
_remotePrefferedAspectRatioUpdated(std::move(descriptor.remotePrefferedAspectRatioUpdated)),
_signalingDataEmitted(std::move(descriptor.signalingDataEmitted)),
_signalBarsUpdated(std::move(descriptor.signalBarsUpdated)),
_enableHighBitrateVideo(descriptor.config.enableHighBitrateVideo) {
	assert(_thread->IsCurrent());
	assert(_stateUpdated != nullptr);
	assert(_signalingDataEmitted != nullptr);
    
    _preferredCodecs = descriptor.config.preferredVideoCodecs;

	_sendSignalingMessage = [=](const Message &message) {
		if (const auto prepared = _signaling.prepareForSending(message)) {
			_signalingDataEmitted(prepared->bytes);
			return prepared->counter;
		}
		return uint32_t(0);
	};
	_sendTransportMessage = [=](Message &&message) {
		_networkManager->perform(RTC_FROM_HERE, [message = std::move(message)](NetworkManager *networkManager) {
			networkManager->sendMessage(message);
		});
	};
}

Manager::~Manager() {
	assert(_thread->IsCurrent());
}

void Manager::sendSignalingAsync(int delayMs, int cause) {
	auto task = [weak = std::weak_ptr<Manager>(shared_from_this()), cause] {
		const auto strong = weak.lock();
		if (!strong) {
			return;
		}
		if (const auto prepared = strong->_signaling.prepareForSendingService(cause)) {
			strong->_signalingDataEmitted(prepared->bytes);
		}
	};
	if (delayMs) {
		_thread->PostDelayedTask(RTC_FROM_HERE, std::move(task), delayMs);
	} else {
		_thread->PostTask(RTC_FROM_HERE, std::move(task));
	}
}

void Manager::start() {
	const auto weak = std::weak_ptr<Manager>(shared_from_this());
	const auto thread = _thread;
	const auto sendSignalingMessage = [=](Message &&message) {
		thread->PostTask(RTC_FROM_HERE, [=, message = std::move(message)]() mutable {
			const auto strong = weak.lock();
			if (!strong) {
				return;
			}
			strong->_sendSignalingMessage(std::move(message));
		});
	};
	_networkManager.reset(new ThreadLocalObject<NetworkManager>(getNetworkThread(), [weak, thread, sendSignalingMessage, encryptionKey = _encryptionKey, enableP2P = _enableP2P, enableTCP = _enableTCP, enableStunMarking = _enableStunMarking, rtcServers = _rtcServers] {
		return new NetworkManager(
			getNetworkThread(),
			encryptionKey,
			enableP2P,
            enableTCP,
            enableStunMarking,
			rtcServers,
			[=](const NetworkManager::State &state) {
				thread->PostTask(RTC_FROM_HERE, [=] {
					const auto strong = weak.lock();
					if (!strong) {
						return;
					}
                    State mappedState;
                    if (state.isFailed) {
                        mappedState = State::Failed;
                    } else {
                        mappedState = state.isReadyToSendData
                            ? State::Established
                            : State::Reconnecting;
                    }
                    bool isFirstConnection = false;
					if (state.isReadyToSendData) {
						if (!strong->_didConnectOnce) {
							strong->_didConnectOnce = true;
                            isFirstConnection = true;
						}
					}
					strong->_state = mappedState;
					strong->_stateUpdated(mappedState);

					strong->_mediaManager->perform(RTC_FROM_HERE, [=](MediaManager *mediaManager) {
						mediaManager->setIsConnected(state.isReadyToSendData);
					});
                    
                    if (isFirstConnection) {
                        strong->sendInitialSignalingMessages();
                    }
				});
			},
			[=](DecryptedMessage &&message) {
				thread->PostTask(RTC_FROM_HERE, [=, message = std::move(message)]() mutable {
					if (const auto strong = weak.lock()) {
						strong->receiveMessage(std::move(message));
					}
				});
			},
			sendSignalingMessage,
			[=](int delayMs, int cause) {
				const auto task = [=] {
					if (const auto strong = weak.lock()) {
						strong->_networkManager->perform(RTC_FROM_HERE, [=](NetworkManager *networkManager) {
							networkManager->sendTransportService(cause);
							});
					}
				};
				if (delayMs) {
					thread->PostDelayedTask(RTC_FROM_HERE, task, delayMs);
				} else {
					thread->PostTask(RTC_FROM_HERE, task);
				}
			});
	}));
	bool isOutgoing = _encryptionKey.isOutgoing;
	_mediaManager.reset(new ThreadLocalObject<MediaManager>(getMediaThread(), [weak, isOutgoing, protocolVersion = _protocolVersion, thread, sendSignalingMessage, videoCapture = _videoCapture, enableHighBitrateVideo = _enableHighBitrateVideo, signalBarsUpdated = _signalBarsUpdated, preferredCodecs = _preferredCodecs]() {
		return new MediaManager(
			getMediaThread(),
			isOutgoing,
            protocolVersion,
			videoCapture,
			sendSignalingMessage,
			[=](Message &&message) {
				thread->PostTask(RTC_FROM_HERE, [=, message = std::move(message)]() mutable {
					const auto strong = weak.lock();
					if (!strong) {
						return;
					}
					strong->_sendTransportMessage(std::move(message));
				});
			},
            signalBarsUpdated,
            enableHighBitrateVideo,
            preferredCodecs);
	}));
    _networkManager->perform(RTC_FROM_HERE, [](NetworkManager *networkManager) {
        networkManager->start();
    });
	_mediaManager->perform(RTC_FROM_HERE, [](MediaManager *mediaManager) {
		mediaManager->start();
	});
}

void Manager::receiveSignalingData(const std::vector<uint8_t> &data) {
	if (auto decrypted = _signaling.handleIncomingPacket((const char*)data.data(), data.size())) {
		receiveMessage(std::move(decrypted->main));
		for (auto &message : decrypted->additional) {
			receiveMessage(std::move(message));
		}
	}
}

void Manager::receiveMessage(DecryptedMessage &&message) {
	const auto data = &message.message.data;
	if (const auto candidatesList = absl::get_if<CandidatesListMessage>(data)) {
		_networkManager->perform(RTC_FROM_HERE, [message = std::move(message)](NetworkManager *networkManager) mutable {
			networkManager->receiveSignalingMessage(std::move(message));
		});
	} else if (const auto videoFormats = absl::get_if<VideoFormatsMessage>(data)) {
		_mediaManager->perform(RTC_FROM_HERE, [message = std::move(message)](MediaManager *mediaManager) mutable {
			mediaManager->receiveMessage(std::move(message));
		});
    } else if (const auto remoteMediaState = absl::get_if<RemoteMediaStateMessage>(data)) {
		if (_remoteMediaStateUpdated) {
			_remoteMediaStateUpdated(
				remoteMediaState->audio,
				remoteMediaState->video);
		}
        _mediaManager->perform(RTC_FROM_HERE, [video = remoteMediaState->video](MediaManager *mediaManager) {
            mediaManager->remoteVideoStateUpdated(video);
        });
	} else if (const auto remoteBatteryLevelIsLow = absl::get_if<RemoteBatteryLevelIsLowMessage>(data)) {
        if (_remoteBatteryLevelIsLowUpdated) {
			_remoteBatteryLevelIsLowUpdated(remoteBatteryLevelIsLow->batteryLow);
        }
    } else if (const auto remoteNetworkType = absl::get_if<RemoteNetworkTypeMessage>(data)) {
        bool wasCurrentNetworkLowCost = calculateIsCurrentNetworkLowCost();
        _remoteNetworkIsLowCost = remoteNetworkType->isLowCost;
        updateIsCurrentNetworkLowCost(wasCurrentNetworkLowCost);
    } else {
        if (const auto videoParameters = absl::get_if<VideoParametersMessage>(data)) {
            float value = ((float)videoParameters->aspectRatio) / 1000.0;
			if (_remotePrefferedAspectRatioUpdated) {
				_remotePrefferedAspectRatioUpdated(value);
			}
        }
		_mediaManager->perform(RTC_FROM_HERE, [=, message = std::move(message)](MediaManager *mediaManager) mutable {
			mediaManager->receiveMessage(std::move(message));
		});
	}
}

void Manager::setVideoCapture(std::shared_ptr<VideoCaptureInterface> videoCapture) {
	assert(_didConnectOnce);

	if (_videoCapture == videoCapture) {
		return;
	}
    _videoCapture = videoCapture;
    _mediaManager->perform(RTC_FROM_HERE, [videoCapture](MediaManager *mediaManager) {
        mediaManager->setSendVideo(videoCapture);
    });
}

void Manager::setRequestedVideoAspect(float aspect) {
    _mediaManager->perform(RTC_FROM_HERE, [aspect](MediaManager *mediaManager) {
        mediaManager->setRequestedVideoAspect(aspect);
    });
}

void Manager::setMuteOutgoingAudio(bool mute) {
	_mediaManager->perform(RTC_FROM_HERE, [mute](MediaManager *mediaManager) {
		mediaManager->setMuteOutgoingAudio(mute);
	});
}

void Manager::setIncomingVideoOutput(std::shared_ptr<rtc::VideoSinkInterface<webrtc::VideoFrame>> sink) {
	_mediaManager->perform(RTC_FROM_HERE, [sink](MediaManager *mediaManager) {
		mediaManager->setIncomingVideoOutput(sink);
	});
}

void Manager::setIsLowBatteryLevel(bool isLowBatteryLevel) {
    _sendTransportMessage({ RemoteBatteryLevelIsLowMessage{ isLowBatteryLevel } });
}

void Manager::setIsLocalNetworkLowCost(bool isLocalNetworkLowCost) {
    if (isLocalNetworkLowCost != _localNetworkIsLowCost) {
        _networkManager->perform(RTC_FROM_HERE, [isLocalNetworkLowCost](NetworkManager *networkManager) {
            networkManager->setIsLocalNetworkLowCost(isLocalNetworkLowCost);
        });
        
        bool wasCurrentNetworkLowCost = calculateIsCurrentNetworkLowCost();
        _localNetworkIsLowCost = isLocalNetworkLowCost;
        updateIsCurrentNetworkLowCost(wasCurrentNetworkLowCost);
        
        switch (_protocolVersion) {
            case ProtocolVersion::V1:
                if (_didConnectOnce) {
                    _sendTransportMessage({ RemoteNetworkTypeMessage{ isLocalNetworkLowCost } });
                }
                break;
            default:
                break;
        }
    }
}

void Manager::getNetworkStats(std::function<void (TrafficStats, CallStats)> completion) {
    _networkManager->perform(RTC_FROM_HERE, [thread = _thread, weak = std::weak_ptr<Manager>(shared_from_this()), completion = std::move(completion), statsLogPath = _statsLogPath](NetworkManager *networkManager) {
        auto networkStats = networkManager->getNetworkStats();
        
        CallStats callStats;
        networkManager->fillCallStats(callStats);
        
        thread->PostTask(RTC_FROM_HERE, [weak, networkStats, completion = std::move(completion), callStats = std::move(callStats), statsLogPath = statsLogPath] {
            const auto strong = weak.lock();
            if (!strong) {
                return;
            }
            
            strong->_mediaManager->perform(RTC_FROM_HERE, [networkStats, completion = std::move(completion), callStatsValue = std::move(callStats), statsLogPath = statsLogPath](MediaManager *mediaManager) {
                CallStats callStats = std::move(callStatsValue);
                mediaManager->fillCallStats(callStats);
                dumpStatsLog(statsLogPath, callStats);
                completion(networkStats, callStats);
            });
        });
    });
}

bool Manager::calculateIsCurrentNetworkLowCost() const {
    return _localNetworkIsLowCost && _remoteNetworkIsLowCost;
}
void Manager::updateIsCurrentNetworkLowCost(bool wasLowCost) {
    bool isLowCost = calculateIsCurrentNetworkLowCost();
    if (isLowCost != wasLowCost) {
        _mediaManager->perform(RTC_FROM_HERE, [isLowCost](MediaManager *mediaManager) {
            mediaManager->setIsCurrentNetworkLowCost(isLowCost);
        });
    }
}

void Manager::sendInitialSignalingMessages() {
    switch (_protocolVersion) {
        case ProtocolVersion::V1:
            _sendTransportMessage({ RemoteNetworkTypeMessage{ _localNetworkIsLowCost } });
            break;
        default:
            break;
    }
}

} // namespace tgcalls
