#include "service.h"
#include "platform.h"
#include "../../native/src/log.h"
#include "../../native/src/perf.h"
#include <filesystem>
#include <fstream>
#include <future>
#include "startup-timing.h"

namespace app {
std::string ErrorMessage(const net::Response& response) {
  if (!response.status) return "Sem conexão com o servidor.";
  auto code = response.Json()["error"].Str();
  if (code == "bad_credentials") return "Usuário ou senha não conferem.";
  if (code == "rate_limited") return "Tentativas demais. Espere quinze minutos.";
  if (code == "invalid_code") return "Código ou link inválido, vencido ou já usado.";
  if (code == "totp_required") return "Digite o código do app ou um código de recuperação.";
  if (code == "mail_unavailable") return "O envio de e-mail está indisponível. Procure o dono do servidor.";
  if (code == "email_required") return "Confirme seu e-mail para continuar.";
  if (code == "bad_email") return "Digite um e-mail válido.";
  if (code == "email_unavailable") return "Não foi possível usar esse e-mail. Confira o endereço ou procure o dono.";
  if (code == "signup_unavailable") return "Não foi possível usar esses dados. Confira o nome e o e-mail.";
  if (code == "invite_unusable") return "Esse convite não vale mais.";
  if (code == "username_taken") return "Esse nome de usuário já existe.";
  if (code == "bad_username") return "Nome de usuário inválido: use de 3 a 32 letras, números, ponto, traço ou _.";
  if (code == "avatar_too_large") return "A foto é grande demais (o limite é 5 MiB).";
  if (code == "avatar_dimensions") return "A foto excede 8000 pixels de largura ou altura, ou 40 megapixels.";
  if (code == "avatar_type") return "Escolha uma foto JPEG, PNG ou GIF. WebP não é aceito.";
  if (code == "bad_image") return "Não foi possível ler essa foto. Escolha outra imagem.";
  if (code == "avatar_limit") return "Você já mudou a foto dez vezes na última hora. Espere um pouco.";
  if (code == "weak_password") return "A senha precisa ter pelo menos 10 caracteres.";
  if (code == "unauthenticated") return "Sua sessão terminou. Entre de novo.";
  return "Deu algo errado. Tente de novo.";
}
Service::Service(std::string base, std::function<void()> changed, bool persistSession)
    : client_(std::move(base)), changed_(std::move(changed)), persistSession_(persistSession),
      // The short way chooses GET or POST by the body alone: a POST without
      // one went out as a GET, and pinning (POST /pin, no body) answered 404
      // to every pin from Windows (the owner, 02/11/2026). Those go the long way.
      chat_([this](const std::string& method, const std::string& path, const json::Value* body) {
        if (method == "GET" || (method == "POST" && body)) return Request(path, body);
        return ChatRequest(client_, method, path, body);
      }, [this] { Notify(); }, {}, {}, persistSession),
      call_([this](const std::string& method, const std::string& path, const json::Value* body) {
        if (method == "GET" || (method == "POST" && body)) return Request(path, body);
        return ChatRequest(client_, method, path, body);
      }, [this](std::function<void(std::string, json::Value)> send, std::function<void(bool)> status) {
        // The media talks from its own thread; the model only ever runs here,
        // on the worker (direct-call.h).
        // The voice test has the microphone and the output open: the call
        // takes them, and plays the voice itself if the switch is on.
        ReleaseVoiceTest();
        VoiceState voice; { std::lock_guard lock(mu_); voice = state_.voice; }
        return CreateDirectMedia(&client_, voice,
          [this, send = std::move(send)](std::string kind, json::Value payload) {
            Enqueue([send, kind = std::move(kind), payload = std::move(payload)]() mutable { send(std::move(kind), std::move(payload)); });
          },
          [this, status = std::move(status)](bool connected) { Enqueue([status, connected] { status(connected); }); });
      }, [this] { if (call_.View().phase == CallPhase::Idle) EndSidetone(); Notify(); }), worker_([this] { Run(); }) {
  for (int i = 0; i < 4; ++i) pictureWorkers_.emplace_back([this] { PictureRun(); });
  // On the worker, as everything the call's model touches.
  Enqueue([this] {
    chat_.personalChanged = [this] { Enqueue([this] { PersonalAudio(); }); };
    call_.volumes = [this](const std::string& peer) {
      bool mic = true, screen = true;
      chat_.Inspect([&](ChatState const& s) { mic = s.Silenced(peer); screen = s.Silenced(peer, true); });
      std::lock_guard lock(mu_); return std::pair{mic ? 0.0 : state_.voice.Volume(peer), screen ? 0.0 : state_.voice.Volume(peer + "/screen_audio")};
    };
  });
}
Service::~Service() { Stop(); }
void Service::Stop() {
  // Flush behind the final slider job, before stopping can discard the queue.
  if(worker_.joinable() && preferencesTouched_) {
    auto saved=std::make_shared<std::promise<void>>();auto done=saved->get_future();
    Enqueue([this,saved] {try{FlushVoicePreferences(true);}catch(...){}saved->set_value();});
    done.wait();
  }
  { std::lock_guard lock(jobsMu_); stopping_ = true;
    for (auto& weak : uploadCancels_) if (auto cancel = weak.lock()) *cancel = true;
  }
  wake_.notify_all(); pictureWake_.notify_all();
  if (worker_.joinable()) worker_.join();
  for (auto& t : pictureWorkers_) if (t.joinable()) t.join();
  if (persistSession_) chat_.Foreground(false);
  if (socket_) { socket_->Stop(); socket_.reset(); }
  // The worker is gone, so this thread is the model's only one now.
  call_.Disconnect();
  DropVoice();
  client_.ClearCookie();
  SecureZeroMemory(challenge_.data(), challenge_.size()); challenge_.clear();
}
void Service::Enqueue(std::function<void()> job) {
  { std::lock_guard lock(jobsMu_); if (stopping_) return; jobs_.push_back(std::move(job)); }
  wake_.notify_one();
}
void Service::ChatWork(std::function<void(Chat&)> operation) {
  Enqueue([this, operation = std::move(operation)] { operation(chat_); });
}
void Service::ForgotPassword(std::string identifier, std::function<void(net::Response)> complete) {
  Enqueue([this, identifier=std::move(identifier),complete=std::move(complete)]() mutable {
    auto body=json::Value::Object(); body["identifier"]=identifier;
    auto result=Request("/api/password/forgot",&body);
    body={}; SecureZeroMemory(identifier.data(),identifier.size()); identifier.clear();
    if (complete) complete(std::move(result));
  });
}
void Service::Signup(json::Value body, std::function<void(net::Response)> complete) {
  { std::lock_guard lock(mu_); if (state_.busy || state_.signedIn) {
      if (complete) complete({409,R"({"error":"unauthenticated"})"}); return;
    }
    state_.busy=true; state_.error.clear();
  }
  Notify();
  Enqueue([this,body=std::move(body),complete=std::move(complete)]() mutable {
    auto code=InviteCode(body["code"].Str(),client_.base());
    net::Response result{400,R"({"error":"invite_unusable"})"};
    if (!code.empty()) { body["code"]=code; client_.ClearCookie(); result=Request("/api/signup",&body); }
    body={}; SecureZeroMemory(code.data(),code.size()); code.clear();
    auto status=result.status;
    if (status==201 && !client_.cookie().empty()) {
      bool saved=!persistSession_ || SaveSession(client_.base(),client_.cookie());
      { std::lock_guard lock(mu_); state_.error=saved ? "" : "Não foi possível guardar a sessão neste Windows."; }
      logf("invite registration accepted"); Connect();
    } else {
      { std::lock_guard lock(mu_); state_.busy=false; state_.error=ErrorMessage(result); }
      logf("invite registration status %d",status); Notify();
    }
    if (complete) complete(std::move(result));
  });
}
void Service::Account(AccountAction action, json::Value body, std::string id,
    std::function<void(net::Response)> complete) {
  auto session = client_.cookie();
  Enqueue([this, action, body = std::move(body), id = std::move(id), session = std::move(session), complete = std::move(complete)]() mutable {
    net::Response result{401, R"({"error":"unauthenticated"})"};
    if (Read().signedIn && !session.empty() && session == client_.cookie()) {
      switch (action) {
        case AccountAction::ChangeEmail: result = Request("/api/me/email", &body); break;
        case AccountAction::VerifyEmail:
          result = Request("/api/me/email/verify", &body);
          if (result.status==200 && Read().needsEmail) {
            { std::lock_guard lock(mu_); state_.needsEmail=false; }
            OpenSession();
          }
          break;
        case AccountAction::ResendEmail: result = Request("/api/me/email/resend", &body); break;
        case AccountAction::Me: result = Request("/api/me"); break;
        case AccountAction::TotpSetup: result = Request("/api/me/2fa/totp/setup", &body); break;
        case AccountAction::TotpEnable: result = Request("/api/me/2fa/totp/enable", &body); break;
        case AccountAction::EmailEnable: result = Request("/api/me/2fa/email/enable", &body); break;
        case AccountAction::DisableFactor: result = Request("/api/me/2fa/disable", &body); break;
        case AccountAction::Recovery: result = Request("/api/me/2fa/recovery", &body); break;
        case AccountAction::Avatar: result = UploadAvatar(client_, std::filesystem::path(net::Wide(id))); break;
        case AccountAction::RemoveAvatar: result = ChatRequest(client_, "DELETE", "/api/me/avatar"); break;
        case AccountAction::Sessions: result = Request("/api/sessions"); break;
        case AccountAction::RevokeSession:
          if (!id.empty()) result = ChatRequest(client_, "DELETE", "/api/sessions/" + ChatEscape(id));
          break;
        case AccountAction::RevokeOthers: {
          auto empty = json::Value::Object(); result = Request("/api/sessions/revoke-others", &empty); break;
        }
        case AccountAction::Password: result = Request("/api/me/password", &body); break;
      }
    }
    body = {}; SecureZeroMemory(session.data(), session.size()); session.clear();
    if (result.status == 401 && result.Json()["error"].Str()=="unauthenticated") Reconnect();
    if (complete) complete(std::move(result));
  });
}
void Service::ChatSend(std::string channel, std::string text, std::vector<std::string> attachments, int64_t replyTo) {
  if (!chat_.BeginSend(channel, text, replyTo)) return;
  Enqueue([this, channel = std::move(channel), text = std::move(text), attachments = std::move(attachments), replyTo]() mutable {
    chat_.SendQueued(channel, std::move(text), std::move(attachments), replyTo);
  });
}
void Service::ChatForeground(bool visible) {
  Enqueue([this, visible] { if (persistSession_) chat_.Foreground(visible); });
}
void Service::ChatUpload(std::string channel, std::filesystem::path file, UploadProgress progress,
    std::shared_ptr<std::atomic<bool>> cancel, std::function<void(net::Response)> complete) {
  // On the picture workers: their own connections, so a file climbing for
  // minutes never sits in front of a message in the account's queue.
  { std::lock_guard lock(jobsMu_); if (stopping_) return;
    std::erase_if(uploadCancels_, [](const auto& weak) { return weak.expired(); });
    uploadCancels_.push_back(cancel);
    pictureJobs_.push_back({client_.cookie(), [this, channel = std::move(channel), file = std::move(file), progress = std::move(progress),
        cancel = std::move(cancel), complete = std::move(complete)](net::Client& client) {
      if (!cancel) return;
      auto r = chat_.Upload(client, channel, file, progress, *cancel);
      if (complete) complete(std::move(r));
    }}); }
  pictureWake_.notify_one();
}
void Service::ChatImage(std::string url, std::function<void(std::shared_ptr<const std::string>)> complete) {
  { std::lock_guard lock(jobsMu_); if (stopping_) return;
    pictureJobs_.push_back({client_.cookie(), [this, url = std::move(url), complete = std::move(complete)](net::Client& client) {
      auto bytes = chat_.Image(client, url); if (complete) complete(std::move(bytes));
    }}); }
  pictureWake_.notify_one();
}
void Service::PictureRun() {
  perf::NameThread(L"harmony-pictures");
  net::Client client(client_.base()); std::string had;
  for (;;) {
    PictureJob job;
    { std::unique_lock lock(jobsMu_); pictureWake_.wait(lock, [&] { return stopping_ || !pictureJobs_.empty(); });
      if (stopping_) return;
      job = std::move(pictureJobs_.front()); pictureJobs_.pop_front(); }
    // The session the job was asked under, not whichever is current when it
    // runs: an upload queued by one account never goes up as the next one,
    // and a job from before a sign-out fails instead of borrowing a session.
    if (job.cookie != had) { if (job.cookie.empty()) client.ClearCookie(); else client.RestoreCookie(job.cookie); had = job.cookie; }
    try { job.run(client); } catch (...) { logf("picture download failed"); }
  }
}
void Service::Run() {
  perf::NameThread(L"harmony-account");
  for (;;) {
    std::function<void()> job;
    try { chat_.Tick(); } catch (...) { logf("chat refresh failed"); }
    try { call_.Tick(); } catch (...) { logf("call tick failed"); }
    FlushVoicePreferences();
    auto delay=chat_.NextTickDelay(), callDelay=call_.NextTickDelay();
    if(callDelay && (!delay || *callDelay<*delay)) delay=callDelay;
    for(auto at:{noiseSaveAt_,volumesSaveAt_}) if(at) {
      auto remaining=std::max<int64_t>(0,at-int64_t(GetTickCount64()));
      if(!delay || remaining<*delay) delay=remaining;
    }
    { std::unique_lock lock(jobsMu_);
      auto ready=[&] {return stopping_ || !jobs_.empty();};
      if(delay) wake_.wait_for(lock,std::chrono::milliseconds(*delay),ready);
      else wake_.wait(lock,ready);
      if (stopping_) { jobs_.clear(); return; }
      if (jobs_.empty()) continue;
      job = std::move(jobs_.front()); jobs_.pop_front(); }
    try { job(); } catch (...) {
      { std::lock_guard lock(mu_); state_.busy = false; state_.error = "Deu algo errado. Tente de novo."; }
      logf("account operation failed"); Notify();
    }
  }
}
State Service::Read() { std::lock_guard lock(mu_); return state_; }
void Service::Notify() { if (changed_) changed_(); }
void Service::Restore() {
  Enqueue([this] {
    startup::Mark("session-restore-start");
    auto cookie = persistSession_ ? LoadSession(client_.base()) : std::string{};
    bool valid = client_.RestoreCookie(cookie);
    SecureZeroMemory(cookie.data(), cookie.size());
    if (valid) {
      // Connect validates the restored session once. Voice preferences are
      // loaded by OpenSession, after the account gate accepts this session.
      Connect(); return;
    }
    { std::lock_guard lock(mu_); state_.busy = false; }
    Notify();
  });
}
void Service::Login(std::string user, std::string password) {
  { std::lock_guard lock(mu_); if (state_.busy) return; state_.busy = true; state_.error.clear(); }
  Notify();
  Enqueue([this, user = std::move(user), password = std::move(password)]() mutable {
    client_.ClearCookie();
    json::Value body = json::Value::Object(); body["username"] = user; body["password"] = password;
    auto response = Request("/api/login", &body);
    SecureZeroMemory(password.data(), password.size()); SecureZeroMemory(user.data(), user.size());
    // Destroy the request promptly; none of its fields go to diagnostics.
    body = {};
    Result(std::move(response));
  });
}
void Service::Code(std::string code) {
  { std::lock_guard lock(mu_); if (state_.busy || !state_.needsCode) return; state_.busy = true; state_.error.clear(); }
  Notify();
  Enqueue([this, code = std::move(code)]() mutable {
    json::Value body = json::Value::Object(); body["challenge"] = challenge_; body["code"] = code;
    auto response = Request("/api/login/2fa", &body);
    SecureZeroMemory(code.data(), code.size()); body = {};
    Result(std::move(response));
  });
}
void Service::Result(net::Response response) {
  if (response.status == 200 && !client_.cookie().empty()) {
    SecureZeroMemory(challenge_.data(), challenge_.size()); challenge_.clear();
    bool saved = !persistSession_ || SaveSession(client_.base(), client_.cookie());
    { std::lock_guard lock(mu_); state_.needsCode = false;
      state_.error = saved ? "" : "Não foi possível guardar a sessão neste Windows."; }
    logf("signed in"); Connect(); return;
  }
  auto answer = response.Json();
  { std::lock_guard lock(mu_); state_.busy = false;
    if (response.status == 200 && !answer["challenge"].Str().empty()) {
      challenge_ = answer["challenge"].Str(); state_.method = answer["twofa"].Str(); state_.needsCode = true;
    } else {
      state_.error = ErrorMessage(response);
      // The API deliberately uses invalid_code for both bad and expired
      // proofs. Restart for either, and for an exhausted proof (429).
      auto reason = answer["error"].Str();
      if (state_.needsCode && (reason == "invalid_code" || reason == "rate_limited")) ClearChallenge();
    }
  }
  logf("account response status %d", response.status); Notify();
}
void Service::Connect() {
  startup::Mark("account-request-start");
  auto response=Request("/api/me");
  startup::Mark("account-request-end");
  if (response.status!=200) {
    // Keep the encrypted session on transport errors, remove revoked ones.
    if (response.status==401) { if(persistSession_) DeleteSession(); client_.ClearCookie(); }
    { std::lock_guard lock(mu_); state_.busy=false; state_.error=ErrorMessage(response); }
    Notify(); return;
  }
  auto me=response.Json();
  { std::lock_guard lock(mu_); state_.signedIn=true; state_.busy=false;
    state_.needsEmail=!me["user"]["email_verified"].Bool(); state_.emailRequired=me["email_required"].Bool(true); }
  if (Read().needsEmail) { Notify(); return; }
  OpenSession();
}
void Service::SkipEmail() {
  Enqueue([this] {
    auto state=Read(); if (!state.signedIn || !state.needsEmail || state.emailRequired) return;
    { std::lock_guard lock(mu_); state_.needsEmail=false; }
    OpenSession();
  });
}
void Service::OpenSession() {
  startup::Mark("socket-start");
  if (socket_) { socket_->Stop(); socket_.reset(); }
  { std::lock_guard lock(mu_); state_.signedIn = true; state_.busy = false; confirmedPresence_.clear(); }
  if (persistSession_) { std::lock_guard lock(mu_); state_.voice.LoadVolumes(DataDirectory() / L"voice-volumes.json"); state_.voice.LoadNoise(DataDirectory() / L"voice-noise.json"); }
  socket_ = std::make_unique<net::Socket>(&client_,
    [this](const std::string& type, const json::Value& data) {
      { std::lock_guard lock(voiceMu_); if (voice_) voice_->Event(type, data); }
      if (type == "app_release") { std::function<void()> f; { std::lock_guard lock(appReleaseMu_); f = appRelease_; } if (f) f(); return; }
      if (type.rfind("call", 0) == 0) { Enqueue([this, type, data] { CallEvent(type, data); }); return; }
      if (type == "person_preferences") { Enqueue([this] { chat_.Resync(); }); return; }
      chat_.Event(type, data);
      // This is the socket's thread. An event can give the chat a deadline (a
      // "digitando" that has to expire, a refresh to retry) while the worker
      // sleeps with none: an empty job makes it look again.
      if (chat_.NextTickDelay()) Enqueue([] {});
      if (type == "message") MessageEvent(data);
      if (type == "ring") Announce(RoomNotice(NoticeContextNow(), data));
      bool changed;
      { std::lock_guard lock(mu_); changed = state_.model.Event(type, data); }
      if (changed) { logf("room or presence updated"); Notify(); }
    }, [this] { Resync(); }, [this] {
      chat_.Disconnect();
      { std::lock_guard lock(mu_); state_.model.Disconnect(); }
      { std::lock_guard lock(voiceMu_); if (voice_) voice_->Recover("gateway-lost"); }
      Notify();
      Enqueue([this] {
        auto me = Request("/api/me");
        if (me.status == 401) {
          call_.Disconnect();
          chat_.Forget();
          DropVoice();
          if (socket_) { socket_->Stop(); socket_.reset(); }
          if (persistSession_) DeleteSession(); client_.ClearCookie();
          { std::lock_guard lock(mu_); state_ = {}; confirmedPresence_.clear(); state_.busy = false; state_.error = ErrorMessage(me); }
          Notify();
        }
      });
    });
  socket_->Start(); Notify();
}
void Service::Resync() {
  startup::Mark("socket-resync-start");
  // Executed on the receive thread, after the socket opens and before reading
  // buffered events. Thus an event cannot be overwritten by an older GET.
  { std::lock_guard lock(mu_); confirmedPresence_.clear(); }
  ApplyPresence();
  chat_.Resync();
  startup::Mark("chat-resync-end");
  auto channels = Request("/api/channels");
  auto occupants = Request("/api/voice/rooms");
  auto presence = Request("/api/presence");
  auto presenceJSON = presence.Json();
  bool success = false;
  { std::lock_guard lock(mu_);
    if (channels.status == 200 && occupants.status == 200 && state_.model.Snapshot(channels.Json(), occupants.Json())) {
      success = true; state_.syncError.clear();
      ++state_.gatewayConnections;
      state_.model.presence.clear();
      for (auto const& [user, status] : presenceJSON["presence"].Items()) state_.model.presence[user] = status.Str();
    } else { state_.model.Disconnect(); state_.syncError = ErrorMessage(channels.status != 200 ? channels : occupants); }
  }
  if (!success && socket_) socket_->Reconnect();
  if (success) { std::lock_guard lock(voiceMu_); if (voice_) voice_->Recover("gateway"); }
  // On the worker like every other call method; HTTP must not hold this thread.
  if (success) Enqueue([this] { call_.Reconcile(); });
  logf("room snapshot received"); Notify();
  startup::Mark("room-resync-end");
}
void Service::Logout() {
  { std::lock_guard lock(mu_); if (state_.busy) return; state_.busy = true; }
  Notify();
  Enqueue([this] {
    call_.Hangup();
    DropVoice();
    auto body = json::Value::Object();
    auto response = Request("/api/logout", &body);
    if (response.status != 204 && response.status != 200 && response.status != 401) {
      { std::lock_guard lock(mu_); state_.busy = false; state_.error = ErrorMessage(response); }
      Notify(); return; // Retry is possible; never claim revocation offline.
    }
    if (socket_) { socket_->Stop(); socket_.reset(); }
    bool deleted = !persistSession_ || DeleteSession(); client_.ClearCookie();
    chat_.Forget();
    { std::lock_guard lock(mu_); state_ = {}; confirmedPresence_.clear(); state_.busy = false;
      if (!deleted) state_.error = "Sessão revogada, mas não foi possível apagar o arquivo local."; }
    logf("signed out"); Notify();
  });
}
net::Response Service::Request(const std::string& path, const json::Value* body) {
  const bool history=!body && path.rfind("/api/channels/",0)==0 && path.find("/messages")!=std::string::npos;
  if(history)startup::Mark("history-request-start");
  auto result=transport_?transport_(path,body):(body ? client_.Post(path,*body) : client_.Get(path));
  if(history)startup::Mark("history-request-end");
  return result;
}
void Service::ApplyPresence() {
  std::string desired;
  { std::lock_guard lock(mu_);
    if (!state_.signedIn || confirmedPresence_ == desiredPresence_) return;
    desired = desiredPresence_;
  }
  json::Value body = json::Value::Object(); body["state"] = desired; body["busy"] = false;
  auto response = Request("/api/presence", &body);
  if (response.status == 200 || response.status == 204) {
    std::lock_guard lock(mu_); confirmedPresence_ = desired;
  }
}
void Service::Presence(std::string state) {
  { std::lock_guard lock(mu_); desiredPresence_ = std::move(state); }
  Enqueue([this] { ApplyPresence(); });
}
void Service::ClearChallenge() {
  SecureZeroMemory(challenge_.data(), challenge_.size()); challenge_.clear();
  state_.needsCode = false; state_.method.clear();
}
void Service::RestartLogin() {
  { std::lock_guard lock(mu_); if (state_.busy || state_.signedIn) return; state_.busy = true; }
  Enqueue([this] {
    { std::lock_guard lock(mu_); ClearChallenge(); state_.error.clear(); state_.busy = false; }
    Notify();
  });
}
void Service::Reconnect() {
  Enqueue([this] { if (socket_) socket_->Reconnect(); });
}
void Service::DropVoice() {
  std::shared_ptr<VoiceEngine> old;
  { std::lock_guard lock(voiceMu_); old = std::move(voice_); }
  if (old) old->Leave();
  old.reset();
  { std::lock_guard lock(mu_); state_.voice.Leave(); state_.voice.voiceTest=false; }
}
void Service::JoinVoice(std::string id) {
  Enqueue([this, id = std::move(id)] {
    VoiceState selected;
    { std::lock_guard lock(mu_);
      if (!state_.signedIn || id == state_.voice.room) return;
      auto room = std::find_if(state_.model.rooms.begin(), state_.model.rooms.end(), [&](auto& r) { return r.id == id; });
      if (room == state_.model.rooms.end()) return;
      selected = state_.voice; selected.Join(id, room->name);
    }
    DropVoice(); ReleaseVoiceTest();  // selected keeps the switch: the room plays the voice
    if (call_.View().phase != CallPhase::Idle) { logf("entering a room ends the direct call"); call_.Hangup(); }
    { std::lock_guard lock(mu_);
      selected.muted=state_.voice.muted; selected.deafened=state_.voice.deafened;
      state_.voice = selected; state_.error.clear(); }
    Notify();
    PersonalVolumes(selected);
    auto engine = CreateVoiceEngine(&client_, selected, [this](VoiceUpdate update) { VoiceChanged(std::move(update)); });
    { std::lock_guard voiceLock(voiceMu_); std::lock_guard stateLock(mu_);
      voice_ = engine;
      if(voice_) voice_->Mute(state_.voice.muted,state_.voice.deafened);
    }
    std::string reason;
    if (!engine || !engine->Start(&reason)) {
      DropVoice();
      { std::lock_guard lock(mu_); state_.error = "Não foi possível entrar na sala de voz."; }
      logf("voice join failed"); Notify();
    } else logf("voice entered");
  });
}
void Service::LeaveVoice() { Enqueue([this] { DropVoice(); Notify(); }); }
void Service::MuteVoice(bool muted, bool deafened) {
  {
    // Serialize state and media together, including publication during JoinVoice.
    std::lock_guard voiceLock(voiceMu_); std::lock_guard stateLock(mu_);
    state_.voice.muted = muted; state_.voice.deafened = deafened;
    if (voice_) voice_->Mute(muted, deafened);
  }
  Notify();
}
void Service::VoiceMicGain(double gain) {
  if(!std::isfinite(gain)) return;
  preferencesTouched_=true;
  Enqueue([this,gain] {
    auto applied=std::clamp(gain,0.,2.);
    { std::lock_guard lock(mu_); state_.voice.micGain=applied;
      if(persistSession_) noiseSaveAt_=int64_t(GetTickCount64())+300; }
    { std::lock_guard lock(voiceMu_); if(voice_) voice_->MicGain(applied); } CallSettings(); Notify();
  });
}
void Service::VoiceMicFilters(bool echo,bool noise,bool agc) {
  Enqueue([this,echo,noise,agc] {
    { std::lock_guard lock(mu_); state_.voice.echo=echo; state_.voice.noise=noise; state_.voice.agc=agc;
      if(persistSession_) { auto root=DataDirectory(); std::error_code ec; std::filesystem::create_directories(root,ec);
        if(!state_.voice.SaveNoise(root/L"voice-noise.json")) state_.error="Não foi possível guardar os filtros."; } }
    { std::lock_guard lock(voiceMu_); if(voice_) voice_->MicFilters(echo,noise,agc); } CallSettings();
    logf("voice filters echo=%d noise=%d agc=%d",echo,noise,agc); Notify();
  });
}
void Service::VoiceNoise(bool enabled,int strength) {
  preferencesTouched_=true;
  Enqueue([this,enabled,strength] {
    auto applied=std::clamp(strength,10,100);
    { std::lock_guard lock(mu_); state_.voice.dfn=enabled; state_.voice.strength=applied; if(!enabled) state_.voice.dfnActive=false;
      if(persistSession_) noiseSaveAt_=int64_t(GetTickCount64())+300; }
    { std::lock_guard lock(voiceMu_); if(voice_) voice_->Noise(enabled,applied); } CallSettings();
    logf("voice DFN preference enabled=%d strength_db=%d",enabled,applied); Notify();
  });
}
void Service::FlushVoicePreferences(bool force) {
  auto now=int64_t(GetTickCount64());
  bool noise=noiseSaveAt_ && (force || now>=noiseSaveAt_);
  bool volumes=volumesSaveAt_ && (force || now>=volumesSaveAt_);
  if(!noise && !volumes) return;
  auto root=DataDirectory();std::error_code error;std::filesystem::create_directories(root,error);
  bool failed=false;
  {
    std::lock_guard lock(mu_);
    if(noise) {noiseSaveAt_=0;if(!state_.voice.SaveNoise(root/L"voice-noise.json")) {state_.error="Não foi possível guardar o filtro.";failed=true;}}
    if(volumes) {volumesSaveAt_=0;if(!state_.voice.SaveVolumes(root/L"voice-volumes.json")) {state_.error="Não foi possível guardar o volume.";failed=true;}}
  }
  if(failed) Notify();
}
void Service::PersonalVolumes(VoiceState& voice) {
  chat_.Inspect([&](ChatState const& s) {
    for (auto const& person : s.members) {
      if (s.Silenced(person.username)) voice.Volume(person.username, 0);
      if (s.Silenced(person.username, true)) voice.Volume(person.username + "/screen_audio", 0);
    }
  });
}
void Service::PersonalAudio() {
  VoiceState effective;
  { std::lock_guard lock(mu_); effective = state_.voice; }
  // Include default gains so removing a preference restores an uncustomized voice too.
  chat_.Inspect([&](ChatState const& s) { for (auto const& p : s.members) {
    effective.Volume(p.username, effective.Volume(p.username));
    effective.Volume(p.username + "/screen_audio", effective.Volume(p.username + "/screen_audio"));
  } });
  PersonalVolumes(effective);
  { std::lock_guard lock(voiceMu_); if (voice_) for (auto const& [key, gain] : effective.volumes) voice_->Volume(key, gain); }
  call_.VolumesChanged();
}
void Service::VoiceVolume(std::string who, double gain) {
  preferencesTouched_=true;
  Enqueue([this, who = std::move(who), gain] {
    double applied;
    { std::lock_guard lock(mu_); state_.voice.Volume(who, gain); applied = state_.voice.Volume(who);
      if(persistSession_) volumesSaveAt_=int64_t(GetTickCount64())+300;
    }
    PersonalAudio();
    logf("voice receive volume %d percent", int(applied * 100)); Notify();
  });
}
void Service::VoiceDeviceChoice(bool capture, std::string id) {
  Enqueue([this, capture, id = std::move(id)] {
    bool active;
    { std::lock_guard lock(voiceMu_); active=bool(voice_); if (voice_) voice_->Device(capture, id); }
    if(!active) { std::lock_guard lock(mu_); (capture?state_.voice.input:state_.voice.output)=id; }
    call_.Device(capture, id);
    // A running voice test reopens on the new device.
    bool testing; { std::lock_guard lock(voiceMu_); testing=bool(voiceTest_); }
    if(testing) { StopVoiceTest(); TestVoice(true); }
    Notify();
  });
}
void Service::TestVoice(bool on) {
  Enqueue([this,on] {
    if(!on) { StopVoiceTest(); Sidetone(false); return; }
    VoiceState v; { std::lock_guard lock(mu_); v=state_.voice; }
    if(!v.room.empty() || call_.View().phase!=CallPhase::Idle) { Sidetone(true); return; }
    StopVoiceTest();
    auto test=CreateVoiceTest(&client_,v);
    bool running=test && test->Running();
    if(running) { std::lock_guard lock(voiceMu_); voiceTest_=std::move(test); }
    { std::lock_guard lock(mu_); state_.voice.voiceTest=running;
      state_.voice.voiceTestError=running ? "" : "Não foi possível abrir o microfone ou a saída de som."; }
    logf("voice test %s",running ? "on" : "failed to open"); Notify();
  });
}
void Service::StopVoiceTest() {
  ReleaseVoiceTest();
  std::lock_guard lock(mu_); state_.voice.voiceTest=false;
}
void Service::ReleaseVoiceTest() {
  std::unique_ptr<VoiceTest> old; { std::lock_guard lock(voiceMu_); old=std::move(voiceTest_); }
  if(old) logf("voice test off");
}
// In a room or a call (the owner, 29/10/2026: "funcionar durante a chamada
// pra funcionar como retorno de voz"), the switch reaches their microphone
// chain; it stays on with Ajustes closed and ends with the room or call.
void Service::Sidetone(bool on) {
  { std::lock_guard lock(voiceMu_); if(voice_) voice_->Sidetone(on); }
  call_.Sidetone(on);
  { std::lock_guard lock(mu_); state_.voice.voiceTest=on; state_.voice.voiceTestError.clear(); }
  logf("voice test %s",on ? "in the call" : "off"); Notify();
}
void Service::EndSidetone() {
  { std::lock_guard lock(voiceMu_); if(voiceTest_) return; }
  std::lock_guard lock(mu_); if(state_.voice.room.empty()) state_.voice.voiceTest=false;
}
void Service::VoiceDevices() {
  Enqueue([this] {
    { std::lock_guard lock(voiceMu_); if(voice_) return; }
    auto inputs=EnumerateVoiceDevices(true), outputs=EnumerateVoiceDevices(false);
    { std::lock_guard lock(mu_); state_.voice.Devices(true,std::move(inputs)); state_.voice.Devices(false,std::move(outputs)); }
    Notify();
  });
}
void Service::RecoverVoice(std::string reason) {
  Enqueue([this, reason = std::move(reason)] {
    { std::lock_guard lock(voiceMu_); if (voice_) voice_->Recover(reason); }
    if (socket_) socket_->Reconnect();
  });
}
void Service::ShareScreen(HWND window,HMONITOR monitor,int height,int fps,bool sound) {
  Enqueue([=,this]{std::lock_guard lock(voiceMu_);if(voice_)voice_->Screen(window,monitor,height,fps,sound);});
}
void Service::StopScreen() { Enqueue([this]{std::lock_guard lock(voiceMu_);if(voice_)voice_->StopScreen();}); }
void Service::Camera(bool on,std::string id) {Camera(on,std::move(id),false);}
void Service::Camera(bool on,std::string id,bool direct) {
  Enqueue([this,on,id=std::move(id),direct]{
    {std::lock_guard lock(mu_);state_.voice.cameraID=id; if(persistSession_) {auto directory=DataDirectory();std::error_code error;std::filesystem::create_directories(directory,error);state_.voice.SaveNoise(directory/L"voice-noise.json");}}
    if(direct) call_.Camera(on,id);
    else {std::lock_guard lock(voiceMu_);if(voice_)voice_->Camera(on,id);} Notify();
  });
}
void Service::Watch(std::string who,HWND target,bool camera) { Enqueue([this,who=std::move(who),target,camera]{std::lock_guard lock(voiceMu_);if(voice_)voice_->Watch(who,target,camera);}); }
void Service::WatchSize(double width,double dpi,bool visible) { Enqueue([=,this]{std::lock_guard lock(voiceMu_);if(voice_)voice_->WatchSize(width,dpi,visible);}); }
void Service::Video(std::string who,std::string source,std::function<void(webrtc::VideoTrackInterface*)> attach) {Enqueue([this,who=std::move(who),source=std::move(source),attach=std::move(attach)]{std::lock_guard lock(voiceMu_);if(voice_)voice_->Video(who,source,attach);});}
void Service::VideoSize(std::string who,double width,double dpi,bool visible) {Enqueue([=,this]{std::lock_guard lock(voiceMu_);if(voice_)voice_->VideoSize(who,width,dpi,visible);});}
void Service::VoiceChanged(VoiceUpdate update) {
  bool changed = false;
  { std::lock_guard lock(mu_);
    auto& v = state_.voice;
    if (v.room.empty()) return;
    if(update.kind==VoiceUpdate::Step) {v.connectionStep=update.reason;changed=true;}
    else if(update.kind==VoiceUpdate::Media) {
      v.sharingScreen=update.sharingScreen; v.sharingCamera=update.sharingCamera; v.mediaNotice=update.reason; changed=true;
    } else if (update.kind == VoiceUpdate::Level) {
      if(update.own && v.self != update.who) { v.self=update.who; changed=true; }
      if(update.reason=="speaking" || update.reason=="quiet") {
        bool on=update.reason=="speaking" && (!update.own || v.Sending());
        changed=(v.speaking[update.who]!=on) || changed;v.speaking[update.who]=on;
      } else changed = v.Level(update.who, update.amplitude, update.at, update.own) || changed;
    }
    else if (update.kind == VoiceUpdate::Render) { v.renderLevel=update.amplitude; v.renderCallbacks=update.generation; v.renderActive=update.reason=="active"; v.dfnActive=update.dfnActive; v.dfnFallbacks=update.dfnFallbacks; v.noiseFilter=update.noiseFilter; v.noiseReason=update.noiseReason;
      v.connectionIdentity=update.connectionIdentity; v.sessionIdentity=update.sessionIdentity;
      v.captureCallbacks=update.captureCallbacks; v.referenceMissing=update.referenceMissing; v.referenceDropped=update.referenceDropped; v.referenceAge=update.referenceAge; }
    else if (update.kind == VoiceUpdate::Devices) {
      auto previous = update.capture ? v.input : v.output;
      changed = v.Devices(update.capture, std::move(update.devices));
      (update.capture ? v.input : v.output) = update.who;
      auto& status=update.capture ? v.inputStatus : v.outputStatus;
      changed = changed || previous != update.who || status!=update.reason;
      status=update.reason; (update.capture ? v.inputReady : v.outputReady)=update.reason=="recovered";
    } else if (update.kind == VoiceUpdate::Recovering) { v.Recover(update.at); changed = true; }
    else if (update.kind == VoiceUpdate::Flowing) {
      v.sessionGeneration = update.generation;
      changed = v.connecting || v.reconnecting;
      v.Recovered(update.at, true, true);
      if (changed && v.recoveryMs >= 0) logf("voice recovered in %lld ms; RTP growing", v.recoveryMs);
    } else if (update.kind == VoiceUpdate::Failed) { v.Leave(); state_.error = "A chamada caiu. Toque na sala para entrar de novo."; changed = true; }
  }
  if (changed) Notify();
}
// ---------------------------------------------------------------- W6
void Service::CallRing(CallPerson who) {
  Enqueue([this, who = std::move(who)] {
    if (call_.View().phase != CallPhase::Idle) return;
    DropVoice(); Notify();
    call_.Ring(who);
  });
}
void Service::CallAccept() {
  Enqueue([this] {
    if (call_.View().phase != CallPhase::Ringing) return;
    DropVoice(); Notify();
    call_.Accept();
  });
}
void Service::CallDecline() { Enqueue([this] { call_.Decline(); }); }
void Service::CallHangup() { Enqueue([this] { call_.Hangup(); }); }
void Service::CallMute(bool muted) {
  auto id = call_.MuteImmediately(muted);
  Enqueue([this, id] { call_.CommitMute(id); });
}
void Service::CallDeafen(bool deafened) { Enqueue([this, deafened] { call_.Deafen(deafened); }); }
void Service::CallOpen(std::string id) { Enqueue([this, id = std::move(id)] { call_.Open(id); }); }
void Service::CallCamera(bool on) {
  Enqueue([this, on] { std::string id; { std::lock_guard lock(mu_); id = state_.voice.cameraID; } call_.Camera(on, id); });
}
void Service::CallScreen(std::string id, HWND window, HMONITOR monitor, int height, int fps, bool sound) {
  Enqueue([=, this] { call_.Screen(id, window, monitor, height, fps, sound); });
}
void Service::CallStopScreen() { Enqueue([this] { call_.StopScreen(); }); }
void Service::CallVideo(int slot, std::function<void(webrtc::VideoTrackInterface*)> attach) {
  Enqueue([this, slot, attach = std::move(attach)] { call_.Video(slot, attach); });
}
// Ajustes in the middle of a direct call reach it now, not on the next one.
void Service::CallSettings() {
  VoiceState v; { std::lock_guard lock(mu_); v = state_.voice; }
  call_.Filters(v.dfn, v.strength, v.echo, v.noise, v.agc, v.micGain);
  std::lock_guard lock(voiceMu_); if(voiceTest_) voiceTest_->Filters(v.dfn, v.strength, v.echo, v.noise, v.agc, v.micGain);
}
void Service::CallEvent(const std::string& type, const json::Value& data) {
  auto before = call_.View();
  call_.Event(type, data);
  if (type != "call") return;
  // Only a call that is now ringing here is announced: one refused because
  // another is going on never rings.
  auto now = call_.View();
  if (now.phase == CallPhase::Ringing && now.id == data["id"].Str() && before.id != now.id)
    Announce(CallNotice(NoticeContextNow(), now.id, now.peer));
}
void Service::MessageEvent(const json::Value& data) {
  auto message = ParseChatMessage(data);
  if (message.id <= 0) return;
  std::optional<Notice> notice;
  auto context = NoticeContextNow();
  bool sound = false; auto soundContext = context; soundContext.enabled = SoundsOn();
  chat_.Inspect([&](const ChatState& s) {
    if (!s.personalReady || s.Blocked(message.author.username)) return;
    const ChatConversation* where = nullptr;
    for (auto& c : s.conversations) if (c.id == message.channel) where = &c;
    notice = MessageNotice(context, message, where);
    sound = MessageSound(soundContext, message, where, s.active);
  });
  Announce(std::move(notice));
  std::function<void()> play;
  { std::lock_guard lock(noticeMu_);
    const auto now = GetTickCount64();
    if (sound && (!lastMessageSound_ || now - lastMessageSound_ >= 500)) { lastMessageSound_ = now; play = messageSound_; }
  }
  if (play) play();
}
NoticeContext Service::NoticeContextNow() {
  NoticeContext context;
  chat_.Inspect([&](const ChatState& s) { context.me = s.owner; });
  context.seen = seen_.load();
  context.enabled = NoticesOn();
  return context;
}
void Service::Announce(std::optional<Notice> notice) {
  if (!notice) return;
  std::function<void(Notice)> show;
  { std::lock_guard lock(noticeMu_); show = notice_; }
  logf("notice %s", notice->target == Notice::Target::Call ? "call" : notice->target == Notice::Target::Room ? "room" : "conversation");
  if (show) show(std::move(*notice));
}
void Service::OnNotice(std::function<void(Notice)> notice) { std::lock_guard lock(noticeMu_); notice_ = std::move(notice); }
bool Service::NoticesOn() {
  std::lock_guard lock(noticeMu_);
  if (!noticePrefsLoaded_) { noticePrefsLoaded_ = true; if (persistSession_) noticePrefs_.Load(DataDirectory() / L"notices.json"); }
  return noticePrefs_.enabled;
}
void Service::OnMessageSound(std::function<void()> play) { std::lock_guard lock(noticeMu_); messageSound_ = std::move(play); }
bool Service::SoundsOn() {
  NoticesOn(); // Load both preferences together, including older files without sound.
  std::lock_guard lock(noticeMu_); return noticePrefs_.sound;
}
void Service::SetSoundsOn(bool on) {
  NoticesOn();
  std::lock_guard lock(noticeMu_); noticePrefs_.sound = on;
  if (persistSession_) {
    std::error_code error; std::filesystem::create_directories(DataDirectory(), error);
    if (!noticePrefs_.Save(DataDirectory() / L"notices.json")) logf("sound preference not saved");
  }
}
void Service::SetNoticesOn(bool on) {
  NoticesOn();
  std::lock_guard lock(noticeMu_);
  noticePrefsLoaded_ = true; noticePrefs_.enabled = on;
  if (persistSession_) {
    std::error_code error; std::filesystem::create_directories(DataDirectory(), error);
    if (!noticePrefs_.Save(DataDirectory() / L"notices.json")) logf("notices preference not saved");
  }
  logf("notices %s", on ? "on" : "off");
}
}
