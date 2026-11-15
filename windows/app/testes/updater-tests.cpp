#define HARMONY_UPDATER_TESTS
#include "../src/updater.h"
#include <cstdlib>
#include <iostream>
#include <stdexcept>
namespace mig=app::installer;
void Check(bool ok,const char* label) { if(!ok) throw std::runtime_error(label); std::cout<<"PASS: "<<label<<'\n'; }
void StopStressTest() {
  using namespace std::chrono;
  std::mutex mutex; std::condition_variable finished; bool done=false;
  std::thread watchdog([&] {
    std::unique_lock lock(mutex);
    if(!finished.wait_for(lock,seconds(30),[&]{return done;})) {
      std::cerr<<"FAIL: 5000 updater destructors exceeded 30 seconds\n"; std::_Exit(1);
    }
  });
  mig::Updater::Timing timing; timing.initial=minutes(10);
  auto start=steady_clock::now();
  for(int i=0;i<5000;++i) {
    mig::Updater updater([](auto const&) {return true;},timing);
    if(i%3==0) std::this_thread::yield();
    if(i%3==1) std::this_thread::sleep_for(microseconds(1));
  }
  {std::lock_guard lock(mutex);done=true;}finished.notify_one();watchdog.join();
  Check(steady_clock::now()-start<seconds(30),"5000 long-delay updater destructors finish within 30 seconds");
}
void ScheduleTests() {
  using namespace std::chrono;
  mig::Updater::Timing timing;
  Check(timing.initial==seconds(60) && timing.retry==minutes(15) && timing.success==hours(6),"production update intervals are 60 seconds, 15 minutes on failure, 6 hours on success");
  Check(timing.login==seconds(2) && timing.cooldown==minutes(1),"login checks have a short delay and a one-minute burst limit");
  timing.initial=milliseconds(400);timing.retry=milliseconds(160);timing.success=milliseconds(600);timing.login=milliseconds(20);timing.cooldown=milliseconds(400);
  std::mutex mutex;std::condition_variable changed;std::vector<steady_clock::time_point> times;
  auto wait=[&](size_t count,milliseconds timeout) {std::unique_lock lock(mutex);return changed.wait_for(lock,timeout,[&]{return times.size()>=count;});};
  {
    mig::Updater updater([&](auto const&) {
      size_t count;{std::lock_guard lock(mutex);times.push_back(steady_clock::now());count=times.size();}changed.notify_all();return count>1;
    },timing);
    Check(wait(1,milliseconds(800)),"opening without a session performs its first deferred check");
    auto login=steady_clock::now();updater.SessionChanged(true);
    for(int i=0;i<100;i++)updater.SessionChanged(true);
    Check(wait(2,milliseconds(140)),"successful login wakes the updater before the failed-check retry interval");
    {std::lock_guard lock(mutex);Check(times[1]-login<milliseconds(140),"login check happens promptly");}
    for(int i=0;i<10;i++){updater.SessionChanged(false);updater.SessionChanged(true);}
    std::this_thread::sleep_for(milliseconds(100));
    {std::lock_guard lock(mutex);Check(times.size()==2,"repeated renders and rapid relogins do not hammer the server");}
    Check(wait(3,milliseconds(650)),"a successful check schedules the longer interval");
    {std::lock_guard lock(mutex);Check(times[2]-times[1]>=milliseconds(550),"success does not retain the short failure interval");}
  }
  times.clear();timing.initial=hours(1);timing.announce=milliseconds(200);
  {
    mig::Updater updater([&](auto const&) {
      {std::lock_guard lock(mutex);times.push_back(steady_clock::now());}changed.notify_all();return true;
    },timing);
    auto announced=steady_clock::now();updater.ReleaseAnnounced();
    Check(wait(1,milliseconds(600)),"a build announced by the server is checked without waiting for the long interval");
    {std::lock_guard lock(mutex);Check(times[0]-announced<milliseconds(400),"the announced check waits at most the spread window");}
  }
  Check(mig::Updater::Timing{}.announce==seconds(30),"announced checks spread over 30 seconds so friends do not download at once");
  times.clear();timing.initial=milliseconds(20);
  {
    mig::Updater updater([&](auto const&) {
      size_t count;{std::lock_guard lock(mutex);times.push_back(steady_clock::now());count=times.size();}changed.notify_all();
      if(count==1)throw std::runtime_error("network failure");return true;
    },timing);
    Check(wait(2,milliseconds(650)),"network failure is retried at the short interval");
    {std::lock_guard lock(mutex);auto gap=times[1]-times[0];Check(gap>=milliseconds(140) && gap<milliseconds(450),"failure retry is bounded and does not busy-loop");}
  }
}
int main(int argc,char** argv) {
  try {
    auto root=std::filesystem::absolute(L"test-output/update-transport");std::filesystem::create_directories(root);
    if(argc==2 && std::string(argv[1])=="--progress") {
      root=app::installer::TestRoot()/L"native";std::filesystem::create_directories(root);
      {std::ofstream out(root/L"current.json");out<<"{\"version\":9}";}
      std::atomic<bool> stop{false};bool downloading=false,ready=false;int percent=-1;
      Check(mig::CheckUpdate(root,stop,[&](mig::UpdateState state) {
        if(state.phase==mig::UpdatePhase::Downloading){Check(state.Percent()>=percent,"progress never goes backwards");percent=state.Percent();downloading=true;}
        if(state.phase==mig::UpdatePhase::Ready)ready=state.release==10;
      }),"local release check completes");
      Check(downloading && percent==100 && ready,"newer distribution is ready only after the full verified download");return 0;
    }
    if(argc==2 && std::string(argv[1])=="--download") {
      std::string text;std::getline(std::cin,text);auto release=json::Parse(text);if(!release)return 2;
      net::Client client(app::Environment(L"HARMONY_URL"));
      if(client.host()!=L"127.0.0.1")return 2;
      client.RestoreCookie("harmony_session="+std::string(43,'A'));
      std::atomic<bool> stop{false};auto file=root/L"candidate.exe";
      DeleteFileW(file.c_str());
      bool ok=mig::ReleaseValid(*release,1)&&mig::Download(client,*release,file,stop);
      std::cout<<(ok?"accepted":"rejected")<<'\n';return 0;
    }
    auto sample=root/L"hash.bin";{std::ofstream out(sample,std::ios::binary);out<<"abc";}
    Check(mig::SHA256(sample)=="ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad","SHA-256 known vector");
    auto release=json::Value::Object();release["version_code"]=27;release["size"]=10;release["sha256"]=std::string(64,'a');release["url"]="/api/app/windows/harmony-27-native.exe";
    Check(mig::UpdateState{mig::UpdatePhase::Downloading,27,25,100}.Percent()==25,"download progress uses verified advertised total");
    Check(mig::UpdateState{mig::UpdatePhase::Downloading,27,110,100}.Percent()==100,"download progress cannot overflow the bar");
    Check(mig::UpdateState{}.Percent()==0,"unknown total does not divide by zero");
    Check(mig::ReleaseValid(release,26),"valid newer same-origin release accepted");
    Check(!mig::ReleaseValid(release,27),"same and older releases rejected");
    for(auto url:{"https://evil.invalid/payload.exe","//evil.invalid/a.exe","/api/app/windows/../escape.exe","/api/app/windows/harmony-27-native.exe?redirect=1","/api/app/windows/harmony-27-native.exe#x"}) {
      auto bad=release;bad["url"]=url;Check(!mig::ReleaseValid(bad,26),"foreign or ambiguous release URL rejected");
    }
    for(int64_t size:{int64_t(-1),int64_t(0),int64_t(512LL*1024*1024+1)}){auto bad=release;bad["size"]=size;Check(!mig::ReleaseValid(bad,26),"invalid download size rejected");}
    auto bad=release;bad["sha256"]="not-a-hash";Check(!mig::ReleaseValid(bad,26),"malformed checksum rejected");
    Check(std::wstring(app::AppID)==L"taleshernandes.harmony","native retains legacy Windows notification identity");
    StopStressTest();
    ScheduleTests();
    return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<'\n';return 1;}
}
