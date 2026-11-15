#pragma once
#include "rooms.h"
#include "chat.h"
#include "voice-engine.h"
#include "direct-call.h"
#include "notices.h"
#include "invite-input.h"
#include "../../native/src/net.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <filesystem>
#include <memory>

namespace app {
struct State {
  Rooms model;
  bool signedIn = false, busy = true;
  std::string error, syncError, method;
  bool needsCode = false;
  bool needsEmail = false, emailRequired = true;
  int gatewayConnections = 0;
  VoiceState voice;
};
class Service {
 public:
  explicit Service(std::string base, std::function<void()> changed, bool persistSession = true);
  ~Service();
  // The server announced a newer build (gateway "app_release").
  void OnAppRelease(std::function<void()> f) { std::lock_guard lock(appReleaseMu_); appRelease_ = std::move(f); }
  State Read();
  void Restore();
  void Login(std::string user, std::string password);
  void Code(std::string code);
  void RestartLogin();
  void ForgotPassword(std::string identifier, std::function<void(net::Response)> complete);
  void Signup(json::Value body, std::function<void(net::Response)> complete);
  void SkipEmail();
  enum class AccountAction { Sessions, RevokeSession, RevokeOthers, Password, Avatar, RemoveAvatar, Me, TotpSetup, TotpEnable, EmailEnable, DisableFactor, Recovery, ChangeEmail, VerifyEmail, ResendEmail };
  // Runs on the account worker, bound to the session that requested it.
  void Account(AccountAction action, json::Value body, std::string id,
      std::function<void(net::Response)> complete);
  void Logout();
  void Presence(std::string state);
  void Reconnect();
  void Stop();
  // Read indexed messages under Chat::Inspect; queue network operations here.
  Chat& ChatModel() { return chat_; }
  void ChatWork(std::function<void(Chat&)> operation);
  // Reserve the busy flag on the caller before enqueueing: two Enter presses
  // before the worker starts must still result in only one HTTP request.
  void ChatSend(std::string channel, std::string text, std::vector<std::string> attachments = {}, int64_t replyTo = 0);
  void ChatForeground(bool visible);
  void ChatUpload(std::string channel, std::filesystem::path file,
      UploadProgress progress, std::shared_ptr<std::atomic<bool>> cancel,
      std::function<void(net::Response)> complete);
  void ChatImage(std::string url, std::function<void(std::shared_ptr<const std::string>)> complete);
  void JoinVoice(std::string id);
  void LeaveVoice();
  void MuteVoice(bool muted, bool deafened);
  void VoiceMicGain(double gain);
  void VoiceMicFilters(bool echo, bool noise, bool agc);
  void VoiceNoise(bool enabled,int strength);
  void VoiceVolume(std::string who, double gain);
  void VoiceDeviceChoice(bool capture, std::string id);
  void VoiceDevices();
  // "Ouvir minha voz": on only outside rooms and direct calls; entering one,
  // or closing Ajustes (the window's job), turns it off.
  void TestVoice(bool on);
  void ShareScreen(HWND,HMONITOR,int,int,bool);
  void StopScreen();
  void Camera(bool,std::string);
  void Camera(bool,std::string,bool direct);
  void Watch(std::string,HWND,bool camera=false);
  void WatchSize(double,double,bool);
  void Video(std::string,std::string,std::function<void(webrtc::VideoTrackInterface*)>);
  void VideoSize(std::string,double,double,bool);
  void RecoverVoice(std::string reason);
  // The direct call (W6). One thing using the microphone at a time: ringing
  // or answering leaves the voice room, and entering a room ends the call.
  CallView CallRead() const { return call_.View(); }
  void CallRing(CallPerson who);
  void CallAccept();
  void CallDecline();
  void CallHangup();
  void CallMute(bool muted);
  void CallDeafen(bool deafened);
  // Opened from a notice: asks the server whether it is still ringing.
  void CallOpen(std::string id);
  void CallCamera(bool on);
  void CallScreen(std::string id, HWND window, HMONITOR monitor, int height, int fps, bool sound);
  void CallStopScreen();
  // A tile for the other person's camera (slot 1) or screen (slot 2); an
  // empty attach lets it go.
  void CallVideo(int slot, std::function<void(webrtc::VideoTrackInterface*)> attach);
  // Windows notices (W6). The window says whether somebody is looking at it;
  // the callback runs on a network thread and only gets who and where.
  void WindowSeen(bool seen) { seen_ = seen; }
  void OnNotice(std::function<void(Notice)> notice);
  void OnMessageSound(std::function<void()> play);
  bool SoundsOn();
  void SetSoundsOn(bool on);
  bool NoticesOn();
  void SetNoticesOn(bool on);
 private:
  std::mutex appReleaseMu_; std::function<void()> appRelease_;
  friend struct ServiceTest;
  net::Response Request(const std::string& path, const json::Value* body = nullptr);
  void ApplyPresence();
  void ClearChallenge();
  std::function<net::Response(const std::string&, const json::Value*)> transport_;
  std::string desiredPresence_ = "hidden", confirmedPresence_;
  void Enqueue(std::function<void()> job);
  void Run();
  void Notify();
  void Result(net::Response response);
  void Connect();
  void OpenSession();
  void Resync();
  void DropVoice();
  void PersonalAudio();
  void PersonalVolumes(VoiceState& voice);
  void VoiceChanged(VoiceUpdate update);
  std::mutex voiceMu_;
  std::shared_ptr<VoiceEngine> voice_;
  std::unique_ptr<VoiceTest> voiceTest_;  // under voiceMu_
  void StopVoiceTest();
  void ReleaseVoiceTest();  // the monitor only; the switch stays as it was
  void Sidetone(bool on);
  void EndSidetone();
  net::Client client_;
  std::function<void()> changed_;
  std::mutex mu_, jobsMu_;
  State state_;
  std::string challenge_;
  std::unique_ptr<net::Socket> socket_;
  std::deque<std::function<void()>> jobs_;
  std::condition_variable wake_;
  bool stopping_ = false;
  bool persistSession_ = true;
  Chat chat_;
  DirectCall call_;
  std::atomic<bool> seen_{false};
  std::mutex noticeMu_;
  std::function<void(Notice)> notice_;
  std::function<void()> messageSound_;
  ULONGLONG lastMessageSound_ = 0;
  NoticePreferences noticePrefs_;
  bool noticePrefsLoaded_ = false;
  void Announce(std::optional<Notice> notice);
  void CallEvent(const std::string& type, const json::Value& data);
  void MessageEvent(const json::Value& data);
  void CallSettings();
  void FlushVoicePreferences(bool force=false);
  int64_t noiseSaveAt_=0, volumesSaveAt_=0; // account worker only
  std::atomic<bool> preferencesTouched_{false};
  NoticeContext NoticeContextNow();
  std::thread worker_;
  // Pictures (Twemoji, photos, files to save) on four workers of their own,
  // each with its own connection carrying the account's cookie: the web's
  // browser fetches them side by side too, and none waits behind the account.
  void PictureRun();
  struct PictureJob { std::string cookie; std::function<void(net::Client&)> run; };
  std::deque<PictureJob> pictureJobs_;
  std::vector<std::weak_ptr<std::atomic<bool>>> uploadCancels_; // under jobsMu_
  std::condition_variable pictureWake_;
  std::vector<std::thread> pictureWorkers_;
};
std::string ErrorMessage(const net::Response& response);
int SelfTest(const std::filesystem::path& output, const std::string& base);
int VoiceSelfTest(const std::filesystem::path& output, const std::string& base);
int ScreenSelfTest(const std::filesystem::path& output, const std::string& base);
}
