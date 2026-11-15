#include <winsock2.h>
#include <ws2tcpip.h>
#include <shobjidl.h>
#include <propkey.h>
#include <propvarutil.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include "../src/rooms.h"
#include "../src/service.h"
#include <fstream>
#include <chrono>
#include "../src/platform.h"
#include "../src/diagnostic-history.h"
#include "../../native/src/log.h"
#include <cstdio>
#include <iostream>
#include <future>
#include <stdexcept>

void RunVoiceTests(void (*check)(bool, const char*));
namespace {
int assertions = 0;
void Check(bool value, const char* name) {
  ++assertions; if (!value) throw std::runtime_error(name);
}
json::Value Parse(const char* text) { auto value = json::Parse(text); if (!value) throw std::runtime_error("invalid fixture"); return *value; }
}
namespace app {
extern std::function<std::shared_ptr<VoiceEngine>(const VoiceState&)> testVoiceFactory;
extern std::function<std::unique_ptr<DirectMedia>()> testDirectFactory;
struct MuteDirect final : DirectMedia {
  std::atomic<bool>* muted;
  explicit MuteDirect(std::atomic<bool>* m) : muted(m) {}
  bool Open(bool, const json::Value&, std::string*) override { return true; }
  void Remote(const std::string&, const json::Value&) override {}
  void Mute(bool value) override { *muted = value; }
  void Close() override {}
};
struct MuteEngine final : VoiceEngine {
  std::function<void()> leaving;
  std::atomic<bool> muted{false},deafened{false},started{false};
  bool startedMuted=false,startedDeafened=false;
  bool Start(std::string*) override {startedMuted=muted;startedDeafened=deafened;started=true;return true;}
  void Leave() override {if(leaving)leaving();}
  void Event(const std::string&,const json::Value&) override {}
  void Recover(const std::string&) override {}
  void Mute(bool m,bool d) override {muted=m;deafened=d;}
  void Volume(const std::string&,double) override {}
  void Device(bool,const std::string&) override {}
};
struct ServiceTest {
  // Test-only pipe protocol. Replies go solely to the owning harness; they
  // never enter the diagnostic ring or a log file.
  static int AccountBridge() {
    Service service(Environment(L"HARMONY_URL"), [] {}, false);
    service.client_.RestoreCookie(Environment(L"HARMONY_TEST_COOKIE"));
    { std::lock_guard lock(service.mu_); service.state_.signedIn=true; service.state_.busy=false; }
    std::string line;
    while (std::getline(std::cin,line)) {
      auto command=json::Parse(line); if (!command) return 2;
      std::promise<net::Response> promise; auto reply=promise.get_future();
      auto complete=[&promise](net::Response r) { promise.set_value(std::move(r)); };
      if ((*command)["action"].Int()==15) {
        { std::lock_guard lock(service.mu_); service.state_.signedIn=false; service.state_.busy=false; }
        service.Signup((*command)["body"],complete);
      } else service.Account(static_cast<Service::AccountAction>((*command)["action"].Int()),(*command)["body"],(*command)["id"].Str(),complete);
      auto r=reply.get(); auto out=json::Value::Object(); out["status"]=r.status; out["body"]=r.body;
      auto logs=json::Value::Array(); for (auto const& entry:LogLines()) logs.Push(entry); out["diagnostics"]=logs;
      std::cout << out.Dump() << std::endl;
    }
    service.Stop(); return 0;
  }

  static void DeferredPreferences() {
    auto root=DataDirectory();std::filesystem::create_directories(root);
    auto noise=root/L"voice-noise.json",volumes=root/L"voice-volumes.json";
    std::filesystem::remove(noise);std::filesystem::remove(volumes);
    Service test("http://127.0.0.1:9",[] {},true);
    auto barrier=[&] {std::promise<void> done;auto future=done.get_future();test.Enqueue([&]{done.set_value();});future.wait();};
    test.VoiceVolume("fixture",.5);test.VoiceMicGain(.6);test.VoiceNoise(true,40);barrier();
    Check(!std::filesystem::exists(noise)&&!std::filesystem::exists(volumes),"slider steps do not write preferences immediately");
    Sleep(350);barrier();
    VoiceState saved;Check(saved.LoadNoise(noise)&&saved.strength==40&&saved.micGain==.6,"noise and gain save after debounce");
    Check(saved.LoadVolumes(volumes)&&saved.Volume("fixture")==.5,"volume saves after debounce");
    test.VoiceVolume("fixture",1.5);test.VoiceMicGain(1.4);test.VoiceNoise(true,70);
    test.Stop();
    Check(saved.LoadNoise(noise)&&saved.strength==70&&saved.micGain==1.4,"shutdown flushes the final queued strength and gain");
    Check(saved.LoadVolumes(volumes)&&saved.Volume("fixture")==1.5,"shutdown flushes the final queued volume");
    std::filesystem::remove(noise);std::filesystem::remove(volumes);
  }
  static void ImmediateMute() {
    using namespace std::chrono;
    {
      std::atomic<int> notices{0};
      Service test("http://127.0.0.1:1",[&] {++notices;},false);
      std::promise<void> entered,release;auto released=release.get_future().share();auto blocked=entered.get_future();
      test.transport_=[&](const std::string&,const json::Value*) -> net::Response {
        entered.set_value();released.wait();return {503,""};
      };
      auto engine=std::make_shared<MuteEngine>();
      {std::lock_guard lock(test.mu_);test.state_.voice.Join("room","Room");}
      {std::lock_guard lock(test.voiceMu_);test.voice_=engine;}
      test.ForgotPassword("fixture",[](auto){});
      bool waiting=blocked.wait_for(seconds(2))==std::future_status::ready;
      auto before=notices.load();
      auto start=steady_clock::now();test.MuteVoice(true,false);
      auto voice=test.Read().voice;
      bool prompt=steady_clock::now()-start<milliseconds(200)&&voice.muted&&engine->muted&&notices>before;
      test.MuteVoice(true,true);
      bool deaf=test.Read().voice.deafened&&engine->deafened;
      release.set_value();test.Stop();
      Check(waiting&&prompt&&deaf,"room mute and deafen apply within 200ms while HTTP worker is blocked");
    }
    {
      Service test("http://127.0.0.1:1",[] {},false);
      std::promise<void> dropping,resumeDrop,creating,resumeCreate,done;
      auto dropped=dropping.get_future();auto drop=resumeDrop.get_future().share();
      auto created=creating.get_future();auto create=resumeCreate.get_future().share();auto completed=done.get_future();
      auto old=std::make_shared<MuteEngine>();old->leaving=[&]{dropping.set_value();drop.wait();};
      auto next=std::make_shared<MuteEngine>();
      testVoiceFactory=[&](const VoiceState&) {creating.set_value();create.wait();return next;};
      {std::lock_guard lock(test.mu_);test.state_.signedIn=true;
        test.state_.voice.Join("old","Old");
        test.state_.model.Snapshot(Parse(R"({"channels":[{"id":"room","name":"Room","kind":"voice"}]})"),Parse(R"({"rooms":{"room":[]}})"));}
      {std::lock_guard lock(test.voiceMu_);test.voice_=old;}
      test.JoinVoice("room");test.Enqueue([&]{done.set_value();});
      bool first=dropped.wait_for(seconds(2))==std::future_status::ready;
      test.MuteVoice(true,false);resumeDrop.set_value();
      bool second=created.wait_for(seconds(2))==std::future_status::ready;
      bool preserved=test.Read().voice.muted;
      test.MuteVoice(false,true);resumeCreate.set_value();
      bool finished=completed.wait_for(seconds(2))==std::future_status::ready;
      auto state=test.Read();test.Stop();testVoiceFactory={};
      Check(first&&second&&preserved,"JoinVoice preserves mute changed after copying selected state");
      Check(finished&&next->started&&!next->startedMuted&&next->startedDeafened&&!next->muted&&next->deafened&&!state.voice.muted&&state.voice.deafened,
        "JoinVoice publishes engine with latest mute and deafen changed during construction");
    }
  }
  // The cancellable upload is asynchronous WinHTTP on its own session: prove a
  // whole multi-block file still arrives byte-complete and the answer is read.
  static void UploadCompletes() {
    WSADATA data{}; Check(WSAStartup(MAKEWORD(2,2), &data)==0,"upload success fixture Winsock");
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    Check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof address)==0 && listen(listener,1)==0,"upload success fixture listener");
    int size=sizeof address;getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size);
    const size_t fileBytes=3*1024*1024+17; std::atomic<uint64_t> declared{0},received{0},marked{0};
    std::thread server([&] {
      SOCKET peer=accept(listener,nullptr,nullptr);
      DWORD timeout=5000;setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof timeout);
      std::string headers;std::vector<char> buffer(65536);
      while(headers.find("\r\n\r\n")==std::string::npos) {
        int n=recv(peer,buffer.data(),int(buffer.size()),0);if(n<=0)break;headers.append(buffer.data(),n);
      }
      auto end=headers.find("\r\n\r\n"),length=headers.find("Content-Length: ");
      if(end!=std::string::npos && length!=std::string::npos) {
        uint64_t total=std::stoull(headers.substr(length+16)),got=headers.size()-end-4,ones=0;
        for(size_t i=end+4;i<headers.size();++i) ones+=headers[i]=='\x5a';
        while(got<total) {
          int n=recv(peer,buffer.data(),int(std::min<uint64_t>(total-got,buffer.size())),0);if(n<=0)break;
          for(int i=0;i<n;++i) ones+=buffer[i]=='\x5a';
          got+=n;
        }
        declared=total;received=got;marked=ones;
      }
      std::string body=R"({"id":"upload-ok","name":"fixture.bin"})";
      std::string answer="HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: "+std::to_string(body.size())+"\r\nConnection: close\r\n\r\n"+body;
      send(peer,answer.data(),int(answer.size()),0);shutdown(peer,SD_SEND);
      while(recv(peer,buffer.data(),int(buffer.size()),0)>0) {}
      closesocket(peer);
    });
    auto file=std::filesystem::absolute(L"test-output/upload-success-fixture.bin");
    {std::ofstream out(file,std::ios::binary);std::string block(fileBytes,'\x5a');out.write(block.data(),std::streamsize(block.size()));}
    net::Response response;uint64_t progressed=0;
    {
      Service test("http://127.0.0.1:"+std::to_string(ntohs(address.sin_port)),[] {},false);
      auto cancel=std::make_shared<std::atomic<bool>>(false);
      HANDLE done=CreateEventW(nullptr,TRUE,FALSE,nullptr);
      test.ChatUpload("upload-fixture",file,[&](uint64_t sent,uint64_t){progressed=sent;},cancel,[&](auto r){response=std::move(r);SetEvent(done);});
      Check(WaitForSingleObject(done,15000)==WAIT_OBJECT_0,"asynchronous upload completes");
      CloseHandle(done);
    }
    server.join();closesocket(listener);WSACleanup();std::filesystem::remove(file);
    Check(response.status==201 && response.Json()["id"].Str()=="upload-ok","completed upload returns the server's answer");
    Check(declared>fileBytes && received==declared && marked>=fileBytes,"every byte of the file reaches the server");
    Check(progressed==fileBytes,"progress reaches the whole file");
  }
  static void CancelUploadOnStop(bool receiveBody) {
    using namespace std::chrono;
    WSADATA data{}; Check(WSAStartup(MAKEWORD(2,2), &data)==0,"upload fixture Winsock");
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    Check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof address)==0 && listen(listener,1)==0,"upload fixture listener");
    int size=sizeof address;getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size);
    HANDLE stalled=CreateEventW(nullptr,TRUE,FALSE,nullptr),finish=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    std::thread server([&] {
      SOCKET peer=accept(listener,nullptr,nullptr);
      DWORD timeout=3000;setsockopt(peer,SOL_SOCKET,SO_RCVTIMEO,reinterpret_cast<char*>(&timeout),sizeof timeout);
      if(receiveBody) {
        std::string headers;char buffer[65536];size_t remaining=0;
        while(headers.find("\r\n\r\n")==std::string::npos) {
          int n=recv(peer,buffer,sizeof buffer,0);if(n<=0)break;headers.append(buffer,n);
        }
        auto end=headers.find("\r\n\r\n"),length=headers.find("Content-Length: ");
        if(end!=std::string::npos && length!=std::string::npos) {
          auto total=std::stoull(headers.substr(length+16));
          remaining=total-(headers.size()-end-4);
          while(remaining) {int n=recv(peer,buffer,int(std::min(remaining,sizeof buffer)),0);if(n<=0)break;remaining-=n;}
        }
      }
      SetEvent(stalled);WaitForSingleObject(finish,3000);closesocket(peer);
    });
    auto file=std::filesystem::absolute(L"test-output/upload-cancel-fixture.bin");
    {std::ofstream out(file,std::ios::binary);out.seekp((receiveBody?1024:32*1024*1024)-1);out.put(0);}
    Service test("http://127.0.0.1:"+std::to_string(ntohs(address.sin_port)),[] {},false);
    test.chat_.Draft("upload-fixture","retained draft");
    auto cancel=std::make_shared<std::atomic<bool>>(false);net::Response response;
    test.ChatUpload("upload-fixture",file,{},cancel,[&](auto r){response=std::move(r);});
    bool waiting=WaitForSingleObject(stalled,5000)==WAIT_OBJECT_0;
    if(!receiveBody)Sleep(100); // let the sender reach a pending write
    auto before=steady_clock::now();test.Stop();auto elapsed=steady_clock::now()-before;
    std::string draft;test.chat_.Inspect([&](const auto& state){draft=state.timelines.at("upload-fixture").draft;});
    SetEvent(finish);server.join();closesocket(listener);CloseHandle(stalled);CloseHandle(finish);WSACleanup();std::filesystem::remove(file);
    Check(waiting && elapsed<seconds(1) && *cancel,"Stop cancels pending upload before joining picture workers");
    Check(!response.status && response.Json()["error"].Str()=="canceled","canceled upload is never reported as sent");
    Check(draft=="retained draft","canceling an upload preserves the unsent draft");
  }
  static void Run() {
    ImmediateMute();
    CancelUploadOnStop(false); CancelUploadOnStop(true); UploadCompletes();
    {
      using namespace std::chrono;
      std::atomic<bool> muted{false};
      testDirectFactory = [&] { return std::make_unique<MuteDirect>(&muted); };
      Service test("http://127.0.0.1:1", [] {}, false);
      test.transport_ = [](const auto&, const auto*) { return net::Response{200,R"({"id":"mute-call","ice_servers":[]})"}; };
      std::promise<void> entered, release;
      auto blocked = entered.get_future(); auto resume = release.get_future().share();
      test.Enqueue([&] { test.call_.Ring({"fixture", "Fixture"}); entered.set_value(); resume.wait(); });
      bool waiting = blocked.wait_for(seconds(2)) == std::future_status::ready;
      auto start = steady_clock::now(); test.CallMute(true);
      bool immediate = muted && steady_clock::now() - start < milliseconds(200);
      test.CallMute(false); test.CallMute(true);
      release.set_value(); test.Stop(); testDirectFactory = {};
      Check(waiting && immediate, "direct mute reaches media while service queue is blocked");
    }
    Check(InviteCode(" invite-fixture ","https://example.invalid")=="invite-fixture","raw invite input trimmed");
    Check(InviteCode("https://example.invalid/signup?code=invite-fixture","https://example.invalid")=="invite-fixture","configured server invite link accepted");
    Check(InviteCode("https://foreign.invalid/signup?code=invite-fixture","https://example.invalid").empty(),"invite cannot change the configured server");
    Check(InviteCode("https://example.invalid/signup","https://example.invalid").empty(),"signup without code rejected");
    for (bool required:{true,false}) {
      Service gate("http://127.0.0.1:1",[] {},false);
      gate.transport_=[required](const std::string&,const json::Value*) -> net::Response {
        return {200,required ? R"({"user":{"email_verified":false},"email_required":true})" : R"({"user":{"email_verified":false},"email_required":false})"};
      };
      gate.Connect();
      Check(gate.Read().signedIn && gate.Read().needsEmail,"unverified sign-in always opens email confirmation");
      gate.SkipEmail(); Sleep(100);
      Check(gate.Read().needsEmail==required,"only optional verification can be skipped");
      gate.Stop();
    }
    {
      // The diagnostic report covers the whole call, not ten minutes.
      auto lines=[](const std::string& s) { size_t n=0; for(char c:s) n+=c=='\n'; return n-2; };  // minus the heading's blank and text lines
      std::vector<DiagnosticRow> rows; std::string out;
      for(int i=0;i<1000;++i) { DiagnosticRow r; r.at=i*2; r.call=i>=400 && i<900 && !(i>=600 && i<610); rows.push_back(r); }
      auto since=CallRowsText(rows,out);
      Check(out.find("desde o começo da chamada")!=std::string::npos && lines(out)==630 && since==1998-740+5,"report starts a minute before the call, across a 20 s gap");
      for(auto& r:rows) r.call=false; out.clear(); CallRowsText(rows,out);
      Check(out.find("últimos 10 min,")!=std::string::npos && lines(out)==300,"report without a call keeps the last ten minutes");
      rows.clear(); for(int i=0;i<5400;++i) { DiagnosticRow r; r.at=i*2; r.call=true; rows.push_back(r); }
      out.clear(); CallRowsText(rows,out);
      Check(lines(out)==2850 && out.find("a cada 4 s")!=std::string::npos,"a three-hour call thins its older rows and keeps the last ten minutes whole");
      logf("diagnostic since probe");
      Check(DiagnosticLogSince(60,1<<20).find("diagnostic since probe")!=std::string::npos && DiagnosticLogSince(60,10).size()<=10,"report log covers recent lines and respects its limit");
    }
    {
      // "Ouvir minha voz": on outside rooms, reopened on a new device, the room's sidetone inside one.
      Service test("http://127.0.0.1:1",[] {},false);
      test.TestVoice(true); Sleep(100);
      Check(test.Read().voice.voiceTest && test.Read().voice.voiceTestError.empty() && test.voiceTest_,"voice test opens outside rooms and calls");
      test.VoiceDeviceChoice(false,"headphones"); Sleep(150);
      Check(test.Read().voice.voiceTest && test.Read().voice.output=="headphones" && test.voiceTest_,"voice test reopens on the chosen output");
      test.TestVoice(false); Sleep(100);
      Check(!test.Read().voice.voiceTest && !test.voiceTest_,"voice test turns off");
      { std::lock_guard lock(test.mu_); test.state_.voice.room="room"; }
      test.TestVoice(true); Sleep(100);
      Check(test.Read().voice.voiceTest && !test.voiceTest_ && test.Read().voice.voiceTestError.empty(),"inside a room the switch is the room's sidetone, without the monitor");
      test.LeaveVoice(); Sleep(100);
      Check(!test.Read().voice.voiceTest,"leaving the room ends the sidetone");
      test.Stop();
    }
    Service service("http://127.0.0.1:1", [] {}, false);
    service.Stop(); // Direct, deterministic execution of the actual handlers.
    for (auto reason : {"invalid_code", "rate_limited"}) {
      service.state_.needsCode = true; service.challenge_ = "synthetic";
      service.Result({reason == std::string("invalid_code") ? 400 : 429,
          "{\"error\":\"" + std::string(reason) + "\"}"});
      Check(!service.Read().needsCode && service.challenge_.empty(), "unusable 2FA proof restarts login");
      Check(service.Read().error == ErrorMessage({400, "{\"error\":\"" + std::string(reason) + "\"}"}), "2FA preserves web message");
    }
    // RestartLogin uses the same queued worker path as the Back button.
    Service back("http://127.0.0.1:1", [] {}, false);
    { std::lock_guard lock(back.mu_); back.state_.busy = false; back.state_.needsCode = true;
      back.state_.method = "totp"; back.challenge_ = "synthetic"; }
    back.RestartLogin();
    for (int i=0; i<100 && back.Read().busy; ++i) Sleep(10);
    Check(!back.Read().needsCode && back.Read().method.empty(), "Back restarts login");
    { std::lock_guard lock(back.mu_); Check(back.challenge_.empty(), "Back wipes proof"); }
    back.Stop();
    service.state_.signedIn = true;
    int posts = 0; bool failPresence = true, failSync = true;
    service.transport_ = [&](const std::string& path, const json::Value* body) -> net::Response {
      if (body) { ++posts; Check((*body)["state"].Str() == service.desiredPresence_, "desired presence reapplied");
        return {failPresence ? 503 : 204, ""}; }
      if (path == "/api/channels") return failSync ? net::Response{503,""} : net::Response{200,R"({"channels":[]})"};
      if (path == "/api/voice/rooms") return {200,R"({"rooms":{}})"};
      return {200,R"({"presence":{}})"};
    };
    service.ApplyPresence(); failPresence = false; service.ApplyPresence(); service.ApplyPresence();
    Check(posts == 2, "presence retries failed POST and caches only success");
    service.state_.error = "session save warning";
    service.Resync(); Check(!service.Read().syncError.empty(), "sync failure shown separately");
    failSync = false; service.Resync();
    Check(service.Read().syncError.empty() && service.Read().model.live, "successful sync clears previous failure");
    Check(service.Read().error == "session save warning", "successful sync retains session warning");
    Check(posts == 4, "every new connection reapplies presence");
    service.desiredPresence_ = "active"; service.ApplyPresence(); service.Resync();
    Check(posts == 6, "visible presence also reapplied after reconnect");
  }
};
}

void SilentSocket(bool upgrade) {
  WSADATA data{}; Check(WSAStartup(MAKEWORD(2,2), &data)==0,"Winsock startup");
  SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  Check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof address)==0 && listen(listener,1)==0,"local silent TCP listener");
  int size=sizeof address; getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size);
  HANDLE accepted=CreateEventW(nullptr,TRUE,FALSE,nullptr), finish=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  std::thread server([&] {
    SOCKET peer=accept(listener,nullptr,nullptr);
    if (upgrade) {
      std::string request; char bytes[4096];
      while (request.find("\r\n\r\n")==std::string::npos) {
        int n=recv(peer,bytes,sizeof bytes,0); if(n<=0) break; request.append(bytes,n);
      }
      auto at=request.find("Sec-WebSocket-Key: ");
      if (at!=std::string::npos) {
        at+=19; auto key=request.substr(at,request.find("\r\n",at)-at)+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        BYTE hash[20]; BCryptHash(BCRYPT_SHA1_ALG_HANDLE,nullptr,0,reinterpret_cast<BYTE*>(key.data()),DWORD(key.size()),hash,sizeof hash);
        char encoded[64]; DWORD length=sizeof encoded;
        CryptBinaryToStringA(hash,sizeof hash,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,encoded,&length);
        auto response=std::string("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: ")+encoded+"\r\n\r\n";
        send(peer,response.data(),int(response.size()),0);
      }
    }
    SetEvent(accepted);
    // No FIN and no frames until Stop returns. A watchdog releases a broken
    // implementation after 3s so the regression test fails without hanging.
    WaitForSingleObject(finish,3000); closesocket(peer);
  });
  net::Client client("http://127.0.0.1:"+std::to_string(ntohs(address.sin_port)));
  std::atomic<bool> opened=false;
  net::Socket live(&client,[](auto const&, auto const&) {},[&] { opened=true; }); live.Start();
  bool connected=WaitForSingleObject(accepted,5000)==WAIT_OBJECT_0;
  for(int i=0; upgrade && !opened && i<100; ++i) Sleep(10);
  Sleep(100); auto begin=std::chrono::steady_clock::now(); live.Stop();
  auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-begin).count();
  SetEvent(finish); server.join(); closesocket(listener); CloseHandle(accepted); CloseHandle(finish); WSACleanup();
  std::printf("silent %s Stop: %lld ms\n",upgrade ? "WebSocket" : "TCP handshake",static_cast<long long>(ms));
  Check(connected && (!upgrade || opened),"silent fixture connected");
  Check(ms<1000,"Stop cancels silent socket in under one second");
}

int main(int argc, char** argv) {
  try {
    if (argc > 1 && std::string(argv[1]) == "--silent-websocket") { SilentSocket(true); return 0; }
    if (argc>1 && std::string(argv[1])=="--account-bridge") return app::ServiceTest::AccountBridge();
    RunVoiceTests(Check);
    app::ServiceTest::DeferredPreferences();
    app::ServiceTest::Run();
    SilentSocket(false); SilentSocket(true);
    Check(SUCCEEDED(CoInitializeEx(nullptr,COINIT_MULTITHREADED)),"shortcut COM initializes");
    auto link=std::filesystem::absolute(L"test-output/start-test.lnk");
    auto target=std::filesystem::absolute(L"test-output/harmony-app-tests.exe");
    Check(app::CreateShortcut(link,target),"shortcut persisted in isolated worktree");
    IShellLinkW* shell=nullptr; IPersistFile* persisted=nullptr; IPropertyStore* props=nullptr;
    Check(SUCCEEDED(CoCreateInstance(CLSID_ShellLink,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&shell))),"shortcut readback COM");
    Check(SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&persisted))) && SUCCEEDED(persisted->Load(link.c_str(),STGM_READ)),"shortcut readback loads disk file");
    Check(SUCCEEDED(shell->QueryInterface(IID_PPV_ARGS(&props))),"shortcut readback property store");
    PROPVARIANT value{};
    Check(SUCCEEDED(props->GetValue(PKEY_AppUserModel_ToastActivatorCLSID,&value)) && value.vt==VT_CLSID && IsEqualGUID(*value.puuid,app::ToastActivator),"shortcut persists classic toast activator"); PropVariantClear(&value);
    Check(SUCCEEDED(props->GetValue(PKEY_AppUserModel_ID,&value)) && value.vt==VT_LPWSTR && std::wstring(value.pwszVal)==app::AppID,"shortcut explicit ID matches process ID"); PropVariantClear(&value);
    for(auto key : {PKEY_AppUserModel_PreventPinning,PKEY_AppUserModel_ExcludeFromShowInNewInstall}) {
      Check(SUCCEEDED(props->GetValue(key,&value)) && value.vt==VT_BOOL && value.boolVal==VARIANT_FALSE,"shortcut exclusion flags persisted false"); PropVariantClear(&value);
    }
    props->Release(); persisted->Release(); shell->Release(); CoUninitialize();
    auto install=std::filesystem::path(L"test-output/startup-fixture");
    std::filesystem::create_directories(install);
    std::ofstream(install/L".harmony-native-install").put('x');
    std::ofstream(install/L"launch.ps1").put('x');
    auto oldCommand=app::StartupCommand(install/L"app-1/harmony-nativo.exe");
    Check(oldCommand==app::StartupCommand(install/L"app-2/harmony-nativo.exe") &&
        oldCommand.find(L"launch.ps1")!=std::wstring::npos && oldCommand.find(L"-Bandeja")!=std::wstring::npos,"startup uses stable launcher from either version");
    Check(app::StartupCommand(L"unpacked/harmony-nativo.exe").empty(),"uninstalled build cannot register startup");
    app::Rooms model;
    auto channels = Parse(R"({"channels":[{"id":"a","name":"Sala A","kind":"voice"},{"id":"b","name":"Sala B","kind":"voice"},{"id":"t","kind":"text"}]})");
    Check(model.Snapshot(channels,Parse(R"({"rooms":{"a":[],"b":[]}})")),"initial snapshot");
    Check(model.live && model.rooms.size()==2,"only voice channels");
    auto join = Parse(R"({"channel_id":"a","peers":[{"username":"test-person","display_name":"Pessoa"}]})");
    Check(model.Event("voice_room",join) && model.rooms[0].people.size()==1,"join");
    Check(model.Event("voice_room",join) && model.rooms[0].people.size()==1,"duplicate event is idempotent");
    Check(model.Event("voice_room",Parse(R"({"channel_id":"a","peers":[]})")) && model.rooms[0].people.empty(),"leave");
    model.Event("voice_room",join);
    model.Event("voice_room",Parse(R"({"channel_id":"a","peers":[]})"));
    model.Event("voice_room",Parse(R"({"channel_id":"b","peers":[{"username":"test-person"}]})"));
    Check(model.rooms[0].people.empty() && model.rooms[1].people.size()==1,"switch rooms");
    model.Event("presence",Parse(R"({"username":"test-person","state":"offline"})"));
    Check(model.rooms[1].people.size()==1,"gateway presence does not guess SFU state");
    Check(model.StateOf("test-person") == "in_call", "voice room presence wins over an offline gateway");
    model.presence["away-person"] = "away";
    Check(model.StateOf("away-person") == "away" && model.StateOf("missing-person") == "offline", "people retain away and offline as separate states");
    model.Event("voice_room",Parse(R"({"channel_id":"b","peers":[]})"));
    Check(model.rooms[1].people.empty(),"SFU disconnect removes peer");
    model.Event("voice_room",join); model.Disconnect();
    Check(!model.live && model.rooms[0].people.size()==1,"observer disconnect keeps last known list");
    model.Event("voice_room",Parse(R"({"channel_id":"a","peers":[{"username":"z1","display_name":"bruno"},{"username":"z2","display_name":"Ágata"},{"username":"z3","display_name":"Carla"},{"username":"z4","display_name":"agnes"},{"username":"a0","display_name":""}]})"));
    { std::string order; for (auto& p : model.rooms[0].people) order += p.username + " ";
      Check(order=="a0 z2 z4 z1 z3 ","call people in alphabetical order, ignoring case and accents"); }
    Check(model.Snapshot(channels,Parse(R"({"rooms":{"a":[],"b":[{"username":"test-person"}]}})")) && model.live && model.rooms[0].people.empty(),"reconnect replaces stale state");
    Check(!model.Event("voice_room",Parse(R"({"channel_id":"b","peers":{}})")) && model.rooms[1].people.size()==1,"malformed event retains state");
    Check(!model.Event("voice_room",Parse(R"({"channel_id":"unknown","peers":[]})")),"unknown room ignored");
    Check(!model.Event("message",Parse(R"({"body":"synthetic private text"})")),"message ignored");
    Check(model.Event("channel_removed",Parse(R"({"id":"b"})")) && model.rooms.size()==1,"room removed");
    Check(model.Event("channel",Parse(R"({"id":"c","name":"Sala C","kind":"voice"})")) && model.rooms.size()==2,"room created");
    Check(!model.Snapshot(channels,Parse(R"({"rooms":[]})")) && model.rooms.size()==2,"malformed snapshot is atomic");
    for (const auto& line : {std::string("Password=synthetic-test"), std::string("TOKEN=")+std::string(43,'X'),
         std::string("test-person@example.invalid"), std::string("code:")+std::string(6,'9'),
         std::string("Cookie: synthetic-test"), std::string("Authorization: synthetic-test"),
         std::string("body=synthetic private text"), std::string("{\"message\":\"synthetic private text\"}"),
         std::string("challenge synthetic-test"), std::string("totp synthetic-test"),
         std::string("innocent\nsynthetic private text"), std::string(43,'X')})
      Check(FilterLog(line)=="[sensitive diagnostic omitted]","secret line omitted");
    for(auto const& line:{"coDE = 123456","short+name@host.invalid","code=0x123z","prefixcode=abc","a@b.c","-------------------------------_","00000000000000000000000000000000"})
      Check(FilterLog(line)=="[sensitive diagnostic omitted]","regex trigger guards preserve sensitive boundaries");
    for(auto const& line:{"code=0xc0000005 ","abcdefghijklmnopqrstuvwxyz12345","decode error 32","a@b","code = 0x1234"})
      Check(FilterLog(line)==line,"regex trigger guards retain safe boundaries");
    Check(FilterLog("socket open")=="socket open","useful diagnostics retained");
    Check(FilterLog("SDP a=ice-pwd: [synthetic]")=="[sensitive diagnostic omitted]","ICE credential line omitted");
    Check(FilterLog("SDP a=ice-ufrag: [synthetic]")=="[sensitive diagnostic omitted]","ICE username fragment omitted");
    Check(FilterLog("account response status 401")=="account response status 401","HTTP status retained");
    const std::string crash="CRASH code=0xc0000005 at module+0x12345678";
    Check(FilterLog(crash)==crash,"real OnCrash format retained");
    Check(FilterLog("code=999999")=="[sensitive diagnostic omitted]","2FA assignment omitted");
    Check(FilterLog("{\"code\":\"999999\"}")=="[sensitive diagnostic omitted]","2FA JSON omitted");
    logf("%s",crash.c_str());
    Check(LogLines().back().find(crash)!=std::string::npos,"actual crash ring retained");
    logf("body=%s","synthetic private text");
    Check(LogLines().back().find("synthetic private text")==std::string::npos,"actual ring filtered before storing");
    if (argc == 1) { std::printf("PASS: %d assertions\n", assertions); return 0; }
    // Optional DPAPI integration test, separate from the required unit suite.
    std::string cookie = "harmony_session=" + std::string(43,'A');
    bool saved = app::SaveSession("http://127.0.0.1:8099",cookie);
    if (!saved) std::fprintf(stderr,"DPAPI Win32 error: 0x%08lx\n",GetLastError());
    Check(saved,"DPAPI save");
    Check(app::LoadSession("http://127.0.0.1:8099")==cookie,"DPAPI restore same origin");
    Check(app::LoadSession("http://127.0.0.1:8100").empty(),"DPAPI rejects other origin");
    Check(app::DeleteSession() && app::LoadSession("http://127.0.0.1:8099").empty(),"session deleted");
    std::printf("PASS: %d assertions\n", assertions); return 0;
  } catch (const std::exception& error) { std::fprintf(stderr,"FAIL: %s (%d assertions)\n",error.what(),assertions); return 1; }
}
