package com.taleshernandes.harmony.model

data class CallPerson(val username: String, val displayName: String) {
    val name: String get() = displayName.ifBlank { username }
}

/** The start, get and accept responses; GET has no ICE servers. */
data class CallInfo(
    val id: String,
    val from: CallPerson,
    val to: CallPerson,
    val iceServers: List<IceServer>,
    val polite: Boolean,
    val answered: Boolean = false,
)

/** A browser session description; type is "offer" or "answer". */
data class Sdp(val type: String, val sdp: String)

/** The browser's RTCIceCandidateInit shape. */
data class Candidate(val candidate: String, val sdpMid: String?, val sdpMLineIndex: Int)

sealed interface CallSignal {
    val callId: String
    data class Description(override val callId: String, val sdp: Sdp) : CallSignal
    data class Ice(override val callId: String, val candidate: Candidate) : CallSignal
}

data class CallIncoming(val callId: String, val from: CallPerson) : GatewayEvent {
    override val type get() = "call"
}

data class CallAccepted(val callId: String, val peer: CallPerson) : GatewayEvent {
    override val type get() = "call_accepted"
}

data class CallTaken(val callId: String, val by: String) : GatewayEvent {
    override val type get() = "call_taken"
}

data class CallEnded(val callId: String, val reason: String) : GatewayEvent {
    override val type get() = "call_ended"
}

data class CallSignalEvent(val signal: CallSignal) : GatewayEvent {
    override val type get() = "call_signal"
}

data class CallStateEvent(val callId: String, val mic: Boolean, val cam: Boolean, val screen: Boolean) : GatewayEvent {
    override val type get() = "call_state"
}
