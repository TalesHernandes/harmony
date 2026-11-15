#include "../../native/gpu/cadence.h"
#include "../../native/src/camera-policy.h"
#include <cstdio>
#include <initializer_list>
#include "w4-timestamp-tests.h"
int main() {
  bool failed=RegularTimestampTests();
  gpu::CaptureClock clock{1000000000};
  // A 50 ms stall in dequeue must not become 50 ms of media timestamp jitter.
  failed|=clock.Timestamp(1016666600)-clock.Timestamp(1000000000)!=16666600;
  uint64_t slot=0;
  failed|=!gpu::GameDue(0,1000000,0,slot)||slot!=1;
  failed|=!gpu::GameDue(100000,1000000,0,slot)||slot!=7;
  failed|=gpu::GameDue(100001,1000000,0,slot);
  failed|=!gpu::GameDue(116667,1000000,0,slot);
  printf("compositor clock preserves 16.667 ms; late game draw skips catch-up burst\n");
  for(int hz:{60,75,144,240}) {
    int64_t capture=0,next[3]{};int counts[3]{};
    for(int frame=0;frame<hz*120;++frame){int64_t now=1000000LL+1000000LL*frame/hz;if(!gpu::CaptureDue(now,60,capture))continue;
      int fps[]{60,30,15};for(int l=0;l<3;++l)if(gpu::LayerDue(now,fps[l],60,next[l]))++counts[l];}
    printf("arrival=%d high=%.3f medium=%.3f low=%.3f\n",hz,counts[0]/120.,counts[1]/120.,counts[2]/120.);
    failed|=counts[0]<57*120||counts[1]<30*120-1||counts[1]>30*120+1||counts[2]<15*120-1||counts[2]>15*120+1;
    // Stopping the other two schedules cannot change the top's cadence.
    capture=0;int64_t highNext=0;int high=0;
    for(int frame=0;frame<hz*120;++frame){int64_t now=1000000LL+1000000LL*frame/hz;if(gpu::CaptureDue(now,60,capture)&&gpu::LayerDue(now,60,60,highNext))++high;}
    failed|=high!=counts[0];
  }
  failed|=gpu::ResolutionRank(1920,1080,1920*1080)!=0||gpu::ResolutionRank(960,540,1920*1080)!=1||gpu::ResolutionRank(480,270,1920*1080)!=2;
  auto c505=camera::WebSize(1280,960),camo=camera::WebSize(720,1280),redmi=camera::WebSize(1280,720);
  failed|=c505.width!=960||c505.height!=720||camo.width!=720||camo.height!=1280||redmi.width!=1280||redmi.height!=720;
  failed|=camera::FormatScore(1280,960,30,0)>=camera::FormatScore(1280,960,7,2)||camera::FormatScore(1280,960,30,0)>=camera::FormatScore(1280,960,30,1);
  failed|=camera::FormatScore(1280,720,7,2)<=camera::FormatScore(1280,960,30,0);
  int64_t deadline=0;int cameraFrames=0;for(int i=0;i<600;++i)if(camera::CameraDue(1000000LL+i*1000000LL/60,deadline))++cameraFrames;
  failed|=cameraFrames<299||cameraFrames>301;printf("C505=960x720 Camo=720x1280 Redmi=1280x720; Camo 60->30 frames=%d/10s\n",cameraFrames);
  return failed?1:0;
}
