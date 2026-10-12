#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/vrrtimingcontroller.h"
#include "../../app/streaming/video/timinggraph.h"
#include <cstdio>
#include <limits>
using Vrr13::InputPercentileBuffer;
int fail=0;
void check(bool b,const char* msg){if(!b){std::fprintf(stderr,"FAIL: %s\n",msg);++fail;}}
void observe(InputPercentileBuffer& b,uint64_t f,int64_t req,int64_t clock,uint64_t room=500,uint64_t window=300000000){b.observe(f,1000000+f*10000,2000000+f*10000,req,true,1000,25000,999500,window,1000,250,0,500,room,clock);}
void rateRoundTrip() {
 VrrSessionConfig cfg;cfg.streamRateHz=116;cfg.displayRefreshHz=120;
 cfg.experimentalInputPercentilePpm=1;cfg.experimentalBoundedCatchup=true;
 cfg.smoothFrameTiming=false;cfg.timingOptions={4000,9995,300,500};
 auto policy=vrrTimingParametersForSession(cfg);policy.playoutDelayStartSeedUs=1000;
 VrrTimingController c(cfg,true,policy);uint64_t slot=1000000,pts=0,submitted=0,frame=0;
 const auto feed=[&](uint64_t count,uint64_t period,uint32_t ticks,uint64_t spike) {
  for(uint64_t n=0;n<count;++n,++frame) {
   slot+=period;pts+=ticks;const uint64_t ready=slot+1000+(frame%20==0?spike:0);
   PacedFrame f(nullptr,int(frame),uint32_t(pts),true,ready);
   f.setDeliveryTimeline(ready-800,ready-600,ready-500);
   const auto d=c.schedule(f,std::max(ready,submitted));
   const uint64_t prepared=std::max(ready,d.renderStartUs)+500;
   submitted=std::max(prepared,d.targetUs);c.notePreparationDuration(500,0,prepared);
   c.noteSchedulerDelays(0,0,true);c.noteSubmission(true,false,submitted,submitted);
  }
 };
 feed(3000,8621,776,5000);const auto fast=c.intervalStats().inputPercentile;
 feed(600,33333,3000,1000);const auto slow=c.intervalStats().inputPercentile;
 feed(600,8621,776,5000);const auto back=c.intervalStats().inputPercentile;
 const uint64_t misses=back.missed-slow.missed,eligible=back.eligible-slow.eligible;
 std::printf("bounded rate round trip: fast %llu slow %llu back %llu, misses %llu/%llu\n",(unsigned long long)fast.currentUs,(unsigned long long)slow.currentUs,(unsigned long long)back.currentUs,(unsigned long long)misses,(unsigned long long)eligible);
 check(eligible>400&&misses*2000<=eligible,"bounded rate return retains 99.95% input coverage without relearning hitches");
 check(slow.currentUs<=fast.currentUs+250,"slower rate invents no additional standing protection");
 check(back.holdRemainingUs==0,"rate transitions introduce no permanent HOLD");
}
int main(){
 VrrSessionConfig cfg;cfg.streamRateHz=76;cfg.displayRefreshHz=120;cfg.latencyMode=0;cfg.experimentalInputPercentilePpm=1;cfg.experimentalBoundedCatchup=true;cfg.timingOptions={4000,9995,300,500};
 auto p=vrrTimingParametersForSession(cfg);VrrTimingController c(cfg,true,p);
 check(p.playoutInputClockRelativeHistory==1&&p.playoutInputBoundedRecoveryCredit==1,"current main enables corrected accounting");
 check(c.recoveryHeadroomUs()>4800&&c.inputRecoveryCreditUs()==500,"large physical gap grants only configured recovery allowance");
 cfg.correctInputAccounting=false;auto old=vrrTimingParametersForSession(cfg);VrrTimingController legacy(cfg,true,old);check(legacy.inputRecoveryCreditUs()==legacy.recoveryHeadroomUs(),"diagnostic legacy control retains original credit");
 check(p.playoutInputPercentileTargetPerMillion==999500&&p.playoutInputPercentileWindowUs==300000000&&p.playoutInputPercentileToleranceUs==500,"active 99.95/300s/0.50ms profile preserved");
 cfg.correctInputAccounting=true;cfg.streamRateHz=116;VrrTimingController high(cfg,true,vrrTimingParametersForSession(cfg));check(high.inputRecoveryCreditUs()==high.recoveryHeadroomUs()&&high.inputRecoveryCreditUs()<500,"near-ceiling physical room retained without over-credit");
 InputPercentileBuffer a,b;a.reset(1000);b.reset(1000);
 for(uint64_t f=1;f<=2500;++f){observe(a,f,5000,100000);observe(b,f,5000,7000000);}check(a.stats().targetUs==4000&&a.stats().targetUs==b.stats().targetUs,"clock origin invariant, tolerance and recovery credit subtracted once");
 const auto historic=a.stats().missed;
 a.advance(26010000,1000,25000,999500,300000000,1000,250,0,500,500,102000);check(a.stats().targetUs==2000&&a.stats().missed==historic,"later mapping removes stale demand, preserves historical outcomes");
 a.advance(26020000,1000,25000,999500,300000000,1000,250,0,500,500,99000);check(a.stats().targetUs==5000,"earlier mapping retains genuinely increased deadline requirement");
 InputPercentileBuffer burst;burst.reset(1000);for(uint64_t f=1;f<=4500;++f)observe(burst,f,f<1000&&f%20<3?5000:0,500000,500,10000000);check(burst.stats().targetUs==1000&&burst.stats().currentUs==1000&&burst.stats().holdRemainingUs==0,"burst expires and drains without permanent HOLD");
 InputPercentileBuffer signs;signs.reset(1000);observe(signs,1,-1000,100000);check(signs.stats().missed==0&&signs.stats().targetUs==1000,"negative normalized readiness invents no demand");observe(signs,2,2000,100000);check(signs.stats().missed==0,"exact B+T+credit equality is covered");observe(signs,3,2001,100000);check(signs.stats().missed==1,"strict boundary counts one microsecond excess");
 InputPercentileBuffer extremes;extremes.reset(1000);observe(extremes,1,std::numeric_limits<int64_t>::max()-5,100);check(extremes.stats().currentUs<=25000&&extremes.stats().clipped,"signed source-coordinate addition saturates at policy cap");extremes.advance(1020000,1000,25000,999500,300000000,1000,250,0,500,500,std::numeric_limits<int64_t>::min());check(extremes.stats().targetUs==25000,"signed projection saturates safely");
 Overlay::TimingGraphPoint g;g.sourceTimingValid=true;g.sourceTimeUs=100000;g.bufferUs=1000;g.inputPercentileEnabled=true;g.producerEligible=true;g.inputToleranceUs=500;g.inputVrrHeadroomUs=500;g.producerRequiredUs=-1000;auto graph=Overlay::bufferGraphSample(g);check(graph.producerUs==-1500&&!graph.producerMissed()&&graph.bufferUs==1000,"signed graph readiness remains separate from standing buffer");
 rateRoundTrip();
 std::printf("main input accounting: %d failures\n",fail);return fail?1:0;
}
