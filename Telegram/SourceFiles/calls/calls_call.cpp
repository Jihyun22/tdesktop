/*
This file is part of Telegram Desktop,
the official desktop version of Telegram messaging app, see https://telegram.org

Telegram Desktop is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

It is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

In addition, as a special exception, the copyright holders give permission
to link the code of portions of this program with the OpenSSL library.

Full license: https://github.com/telegramdesktop/tdesktop/blob/master/LICENSE
Copyright (c) 2014-2017 John Preston, https://desktop.telegram.org
*/
#include "calls/calls_call.h"

#include "auth_session.h"
#include "mainwidget.h"
#include "calls/calls_instance.h"
#include "base/openssl_help.h"

#include <openssl/rand.h>
#include <openssl/sha.h>

#ifdef slots
#undef slots
#define NEED_TO_RESTORE_SLOTS
#endif // slots

#include <VoIPController.h>
#include <VoIPServerConfig.h>

#ifdef NEED_TO_RESTORE_SLOTS
#define slots Q_SLOTS
#undef NEED_TO_RESTORE_SLOTS
#endif // NEED_TO_RESTORE_SLOTS

namespace Calls {
namespace {

constexpr auto kMinLayer = 65;
constexpr auto kMaxLayer = 65; // MTP::CurrentLayer?
constexpr auto kHangupTimeoutMs = 5000; // TODO read from server config

using tgvoip::Endpoint;

void ConvertEndpoint(std::vector<tgvoip::Endpoint> &ep, const MTPDphoneConnection &mtc) {
	if (mtc.vpeer_tag.v.length() != 16) {
		return;
	}
	auto ipv4 = tgvoip::IPv4Address(std::string(mtc.vip.v.constData(), mtc.vip.v.size()));
	auto ipv6 = tgvoip::IPv6Address(std::string(mtc.vipv6.v.constData(), mtc.vipv6.v.size()));
	ep.push_back(Endpoint((int64_t)mtc.vid.v, (uint16_t)mtc.vport.v, ipv4, ipv6, EP_TYPE_UDP_RELAY, (unsigned char*)mtc.vpeer_tag.v.data()));
}

std::vector<gsl::byte> ComputeModExp(const DhConfig &config, const openssl::BigNum &base, const std::array<gsl::byte, Call::kRandomPowerSize> &randomPower) {
	using namespace openssl;

	BigNum resultBN;
	resultBN.setModExp(base, BigNum(randomPower), BigNum(config.p));
	auto result = resultBN.getBytes();
	constexpr auto kMaxModExpSize = 256;
	t_assert(result.size() <= kMaxModExpSize);
	return result;
}

std::vector<gsl::byte> ComputeModExpFirst(const DhConfig &config, const std::array<gsl::byte, Call::kRandomPowerSize> &randomPower) {
	return ComputeModExp(config, openssl::BigNum(config.g), randomPower);
}

std::vector<gsl::byte> ComputeModExpFinal(const DhConfig &config, base::const_byte_span first, const std::array<gsl::byte, Call::kRandomPowerSize> &randomPower) {
	return ComputeModExp(config, openssl::BigNum(first), randomPower);
}

constexpr auto kFingerprintDataSize = 256;
uint64 ComputeFingerprint(const std::array<gsl::byte, kFingerprintDataSize> &authKey) {
	auto hash = openssl::Sha1(authKey);
	return (gsl::to_integer<uint64>(hash[19]) << 56)
		| (gsl::to_integer<uint64>(hash[18]) << 48)
		| (gsl::to_integer<uint64>(hash[17]) << 40)
		| (gsl::to_integer<uint64>(hash[16]) << 32)
		| (gsl::to_integer<uint64>(hash[15]) << 24)
		| (gsl::to_integer<uint64>(hash[14]) << 16)
		| (gsl::to_integer<uint64>(hash[13]) << 8)
		| (gsl::to_integer<uint64>(hash[12]));
}

} // namespace

Call::Call(gsl::not_null<Delegate*> delegate, gsl::not_null<UserData*> user, Type type)
: _delegate(delegate)
, _user(user)
, _type(type) {
	if (_type == Type::Outgoing) {
		setState(State::Requesting);
	}
}

void Call::generateRandomPower(base::const_byte_span random) {
	Expects(random.size() == _randomPower.size());
	memset_rand(_randomPower.data(), _randomPower.size());
	for (auto i = 0, count = int(_randomPower.size()); i != count; i++) {
		_randomPower[i] ^= random[i];
	}
}

void Call::start(base::const_byte_span random) {
	// Save config here, because it is possible that it changes between
	// different usages inside the same call.
	_dhConfig = _delegate->getDhConfig();
	t_assert(_dhConfig.g != 0);
	t_assert(!_dhConfig.p.empty());

	generateRandomPower(random);

	if (_type == Type::Outgoing) {
		startOutgoing();
	} else {
		startIncoming();
	}
}

void Call::startOutgoing() {
	_ga = ComputeModExpFirst(_dhConfig, _randomPower);
	if (_ga.empty()) {
		LOG(("Call Error: Could not compute mod-exp first."));
		setState(State::Failed);
		return;
	}
	_gaHash = openssl::Sha256(_ga);
	auto randomID = rand_value<int32>();

	setState(State::Requesting);
	request(MTPphone_RequestCall(_user->inputUser, MTP_int(randomID), MTP_bytes(_gaHash), MTP_phoneCallProtocol(MTP_flags(MTPDphoneCallProtocol::Flag::f_udp_p2p | MTPDphoneCallProtocol::Flag::f_udp_reflector), MTP_int(kMinLayer), MTP_int(kMaxLayer)))).done([this](const MTPphone_PhoneCall &result) {
		Expects(result.type() == mtpc_phone_phoneCall);
		auto &call = result.c_phone_phoneCall();
		App::feedUsers(call.vusers);
		if (call.vphone_call.type() != mtpc_phoneCallWaiting) {
			LOG(("Call Error: Expected phoneCallWaiting in response to phone.requestCall()"));
			setState(State::Failed);
			return;
		}

		setState(State::Waiting);
		if (_finishAfterRequestingCall) {
			hangup();
			return;
		}

		auto &phoneCall = call.vphone_call.c_phoneCallWaiting();
		_id = phoneCall.vid.v;
		_accessHash = phoneCall.vaccess_hash.v;
		handleUpdate(call.vphone_call);
	}).fail([this](const RPCError &error) {
		setState(State::Failed);
	}).send();
}

void Call::startIncoming() {
	setState(State::Ringing);
}

void Call::answer() {
	Expects(_type == Type::Incoming);
	_gb = ComputeModExpFirst(_dhConfig, _randomPower);
	if (_gb.empty()) {
		LOG(("Call Error: Could not compute mod-exp first."));
		setState(State::Failed);
		return;
	}

	setState(State::ExchangingKeys);
	request(MTPphone_AcceptCall(MTP_inputPhoneCall(MTP_long(_id), MTP_long(_accessHash)), MTP_bytes(_gb), _protocol)).done([this](const MTPphone_PhoneCall &result) {
		Expects(result.type() == mtpc_phone_phoneCall);
		auto &call = result.c_phone_phoneCall();
		App::feedUsers(call.vusers);
		if (call.vphone_call.type() != mtpc_phoneCallWaiting) {
			LOG(("Call Error: Expected phoneCallWaiting in response to phone.acceptCall()"));
			setState(State::Failed);
			return;
		}

		handleUpdate(call.vphone_call);
	}).fail([this](const RPCError &error) {
		setState(State::Failed);
	}).send();
}

void Call::setMute(bool mute) {
	_mute = mute;
	if (_controller) {
		_controller->SetMicMute(_mute);
	}
}

void Call::hangup() {
	auto missed = (_state == State::Ringing || (_state == State::Waiting && _type == Type::Outgoing));
	auto reason = missed ? MTP_phoneCallDiscardReasonMissed() : MTP_phoneCallDiscardReasonHangup();
	finish(reason);
}

void Call::decline() {
	finish(MTP_phoneCallDiscardReasonBusy());
}

bool Call::handleUpdate(const MTPPhoneCall &call) {
	switch (call.type()) {
	case mtpc_phoneCallRequested: {
		auto &data = call.c_phoneCallRequested();
		if (_type != Type::Incoming
			|| _id != 0
			|| peerToUser(_user->id) != data.vadmin_id.v) {
			Unexpected("phoneCallRequested call inside an existing call handleUpdate()");
		}
		if (AuthSession::CurrentUserId() != data.vparticipant_id.v) {
			LOG(("Call Error: Wrong call participant_id %1, expected %2.").arg(data.vparticipant_id.v).arg(AuthSession::CurrentUserId()));
			setState(State::Failed);
			return true;

		}
		_id = data.vid.v;
		_accessHash = data.vaccess_hash.v;
		_protocol = data.vprotocol;
		auto gaHashBytes = bytesFromMTP(data.vg_a_hash);
		if (gaHashBytes.size() != _gaHash.size()) {
			LOG(("Call Error: Wrong g_a_hash size %1, expected %2.").arg(gaHashBytes.size()).arg(_gaHash.size()));
			setState(State::Failed);
			return true;
		}
		base::copy_bytes(gsl::make_span(_gaHash), gaHashBytes);
	} return true;

	case mtpc_phoneCallEmpty: {
		auto &data = call.c_phoneCallEmpty();
		if (data.vid.v != _id) {
			return false;
		}
		LOG(("Call Error: phoneCallEmpty received."));
		setState(State::Failed);
	} return true;

	case mtpc_phoneCallWaiting: {
		auto &data = call.c_phoneCallWaiting();
		if (data.vid.v != _id) {
			return false;
		}
	} return true;

	case mtpc_phoneCall: {
		auto &data = call.c_phoneCall();
		if (data.vid.v != _id) {
			return false;
		}
		if (_type == Type::Incoming && _state == State::ExchangingKeys) {
			startConfirmedCall(data);
		}
	} return true;

	case mtpc_phoneCallDiscarded: {
		auto &data = call.c_phoneCallDiscarded();
		if (data.vid.v != _id) {
			return false;
		}
		if (data.is_need_debug()) {
			auto debugLog = _controller ? _controller->GetDebugLog() : std::string();
			if (!debugLog.empty()) {
				MTP::send(MTPphone_SaveCallDebug(MTP_inputPhoneCall(MTP_long(_id), MTP_long(_accessHash)), MTP_dataJSON(MTP_string(debugLog))));
			}
		}
		if (data.has_reason() && data.vreason.type() == mtpc_phoneCallDiscardReasonBusy) {
			setState(State::Busy);
		} else {
			setState(State::Ended);
		}
	} return true;

	case mtpc_phoneCallAccepted: {
		auto &data = call.c_phoneCallAccepted();
		if (data.vid.v != _id) {
			return false;
		}
		if (_type != Type::Outgoing) {
			LOG(("Call Error: Unexpected phoneCallAccepted for an incoming call."));
			setState(State::Failed);
		} else if (checkCallFields(data)) {
			confirmAcceptedCall(data);
		}
	} return true;
	}

	Unexpected("phoneCall type inside an existing call handleUpdate()");
}

void Call::confirmAcceptedCall(const MTPDphoneCallAccepted &call) {
	Expects(_type == Type::Outgoing);

	// TODO check isGoodGaAndGb
	auto computedAuthKey = ComputeModExpFinal(_dhConfig, byteVectorFromMTP(call.vg_b), _randomPower);
	if (computedAuthKey.empty()) {
		LOG(("Call Error: Could not compute mod-exp final."));
		setState(State::Failed);
		return;
	}

	auto computedAuthKeySize = computedAuthKey.size();
	t_assert(computedAuthKeySize <= kAuthKeySize);
	auto authKeyBytes = gsl::make_span(_authKey);
	if (computedAuthKeySize < kAuthKeySize) {
		base::set_bytes(authKeyBytes.subspan(0, kAuthKeySize - computedAuthKeySize), gsl::byte());
		base::copy_bytes(authKeyBytes.subspan(kAuthKeySize - computedAuthKeySize), computedAuthKey);
	} else {
		base::copy_bytes(authKeyBytes, computedAuthKey);
	}
	_keyFingerprint = ComputeFingerprint(_authKey);

	setState(State::ExchangingKeys);
	request(MTPphone_ConfirmCall(MTP_inputPhoneCall(MTP_long(_id), MTP_long(_accessHash)), MTP_bytes(_ga), MTP_long(_keyFingerprint), MTP_phoneCallProtocol(MTP_flags(MTPDphoneCallProtocol::Flag::f_udp_p2p | MTPDphoneCallProtocol::Flag::f_udp_reflector), MTP_int(kMinLayer), MTP_int(kMaxLayer)))).done([this](const MTPphone_PhoneCall &result) {
		Expects(result.type() == mtpc_phone_phoneCall);
		auto &call = result.c_phone_phoneCall();
		App::feedUsers(call.vusers);
		if (call.vphone_call.type() != mtpc_phoneCall) {
			LOG(("Call Error: Expected phoneCall in response to phone.confirmCall()"));
			setState(State::Failed);
			return;
		}

		createAndStartController(call.vphone_call.c_phoneCall());
	}).fail([this](const RPCError &error) {
		setState(State::Failed);
	}).send();
}

void Call::startConfirmedCall(const MTPDphoneCall &call) {
	Expects(_type == Type::Incoming);

	auto firstBytes = bytesFromMTP(call.vg_a_or_b);
	if (_gaHash != openssl::Sha256(firstBytes)) {
		LOG(("Call Error: Wrong g_a hash received."));
		setState(State::Failed);
		return;
	}

	// TODO check isGoodGaAndGb
	auto computedAuthKey = ComputeModExpFinal(_dhConfig, firstBytes, _randomPower);
	if (computedAuthKey.empty()) {
		LOG(("Call Error: Could not compute mod-exp final."));
		setState(State::Failed);
		return;
	}

	auto computedAuthKeySize = computedAuthKey.size();
	t_assert(computedAuthKeySize <= kAuthKeySize);
	auto authKeyBytes = gsl::make_span(_authKey);
	if (computedAuthKeySize < kAuthKeySize) {
		base::set_bytes(authKeyBytes.subspan(0, kAuthKeySize - computedAuthKeySize), gsl::byte());
		base::copy_bytes(authKeyBytes.subspan(kAuthKeySize - computedAuthKeySize), computedAuthKey);
	} else {
		base::copy_bytes(authKeyBytes, computedAuthKey);
	}
	_keyFingerprint = ComputeFingerprint(_authKey);

	createAndStartController(call);
}

void Call::createAndStartController(const MTPDphoneCall &call) {
	if (!checkCallFields(call)) {
		return;
	}

	setState(State::Established);

	voip_config_t config;
	config.data_saving = DATA_SAVING_NEVER;
	config.enableAEC = true;
	config.enableNS = true;
	config.enableAGC = true;
	config.init_timeout = 30;
	config.recv_timeout = 10;

	std::vector<Endpoint> endpoints;
	ConvertEndpoint(endpoints, call.vconnection.c_phoneConnection());
	for (int i = 0; i < call.valternative_connections.v.length(); i++) {
		ConvertEndpoint(endpoints, call.valternative_connections.v[i].c_phoneConnection());
	}

	_controller = std::make_unique<tgvoip::VoIPController>();
	if (_mute) {
		_controller->SetMicMute(_mute);
	}
	_controller->implData = static_cast<void*>(this);
	_controller->SetRemoteEndpoints(endpoints, true);
	_controller->SetConfig(&config);
	_controller->SetEncryptionKey(reinterpret_cast<char*>(_authKey.data()), (_type == Type::Outgoing));
	_controller->SetStateCallback([](tgvoip::VoIPController *controller, int state) {
		static_cast<Call*>(controller->implData)->handleControllerStateChange(controller, state);
	});
	_controller->Start();
	_controller->Connect();
}

void Call::handleControllerStateChange(tgvoip::VoIPController *controller, int state) {
	// NB! Can be called from an arbitrary thread!
	// Expects(controller == _controller.get()); This can be called from ~VoIPController()!
	Expects(controller->implData == static_cast<void*>(this));

	switch (state) {
	case STATE_WAIT_INIT: {
		DEBUG_LOG(("Call Info: State changed to WaitingInit."));
		setStateQueued(State::WaitingInit);
	} break;

	case STATE_WAIT_INIT_ACK: {
		DEBUG_LOG(("Call Info: State changed to WaitingInitAck."));
		setStateQueued(State::WaitingInitAck);
	} break;

	case STATE_ESTABLISHED: {
		DEBUG_LOG(("Call Info: State changed to Established."));
		setStateQueued(State::Established);
	} break;

	case STATE_FAILED: {
		DEBUG_LOG(("Call Info: State changed to Failed."));
		setStateQueued(State::Failed);
	} break;

	default: LOG(("Call Error: Unexpected state in handleStateChange: %1").arg(state));
	}
}

template <typename T>
bool Call::checkCallCommonFields(const T &call) {
	auto checkFailed = [this] {
		setState(State::Failed);
		return false;
	};
	if (call.vaccess_hash.v != _accessHash) {
		LOG(("Call Error: Wrong call access_hash."));
		return checkFailed();
	}
	auto adminId = (_type == Type::Outgoing) ? AuthSession::CurrentUserId() : peerToUser(_user->id);
	auto participantId = (_type == Type::Outgoing) ? peerToUser(_user->id) : AuthSession::CurrentUserId();
	if (call.vadmin_id.v != adminId) {
		LOG(("Call Error: Wrong call admin_id %1, expected %2.").arg(call.vadmin_id.v).arg(adminId));
		return checkFailed();
	}
	if (call.vparticipant_id.v != participantId) {
		LOG(("Call Error: Wrong call participant_id %1, expected %2.").arg(call.vparticipant_id.v).arg(participantId));
		return checkFailed();
	}
	return true;
}

bool Call::checkCallFields(const MTPDphoneCall &call) {
	if (!checkCallCommonFields(call)) {
		return false;
	}
	if (call.vkey_fingerprint.v != _keyFingerprint) {
		LOG(("Call Error: Wrong call fingerprint."));
		setState(State::Failed);
		return false;
	}
	return true;
}

bool Call::checkCallFields(const MTPDphoneCallAccepted &call) {
	return checkCallCommonFields(call);
}

void Call::setState(State state) {
	if (_state != state) {
		_state = state;
		_stateChanged.notify(state, true);

		switch (_state) {
		case State::WaitingInit:
		case State::WaitingInitAck:
		case State::Established:
			_startTime = getms(true);
			break;
		case State::Ended:
			_delegate->callFinished(this);
			break;
		case State::Failed:
			_delegate->callFailed(this);
			break;
		case State::Busy:
			_hangupByTimeoutTimer.call(kHangupTimeoutMs, [this] { setState(State::Ended); });
			// TODO play sound
			break;
		}
	}
}

void Call::finish(const MTPPhoneCallDiscardReason &reason) {
	if (_state == State::Requesting) {
		_hangupByTimeoutTimer.call(kHangupTimeoutMs, [this] { setState(State::Ended); });
		_finishAfterRequestingCall = true;
		return;
	}
	if (_state == State::HangingUp || _state == State::Ended) {
		return;
	}
	if (!_id) {
		setState(State::Ended);
		return;
	}

	setState(State::HangingUp);
	auto duration = _startTime ? static_cast<int>((getms(true) - _startTime) / 1000) : 0;
	auto connectionId = _controller ? _controller->GetPreferredRelayID() : 0;
	_hangupByTimeoutTimer.call(kHangupTimeoutMs, [this] { setState(State::Ended); });
	request(MTPphone_DiscardCall(MTP_inputPhoneCall(MTP_long(_id), MTP_long(_accessHash)), MTP_int(duration), reason, MTP_long(connectionId))).done([this](const MTPUpdates &result) {
		// This could be destroyed by updates, so we set Ended after
		// updates being handled, but in a guarded way.
		InvokeQueued(this, [this] { setState(State::Ended); });
		App::main()->sentUpdatesReceived(result);
	}).fail([this](const RPCError &error) {
		setState(State::Ended);
	}).send();
}

void Call::setStateQueued(State state) {
	InvokeQueued(this, [this, state] { setState(state); });
}

Call::~Call() {
	if (_controller) {
		DEBUG_LOG(("Call Info: Destroying call controller.."));
		_controller.reset();
		DEBUG_LOG(("Call Info: Call controller destroyed."));
	}
}

} // namespace Calls