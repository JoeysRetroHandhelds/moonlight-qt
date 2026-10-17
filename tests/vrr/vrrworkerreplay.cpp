// Counterfactual admission/service simulation using the actual production worker.
// No window, network, decoder or graphics device is created.
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrrpacingworker.h"
#include "vrrtestfakes.h"
#include "vrrreplayconfig.h"
#include "../../app/streaming/video/ffmpeg-renderers/pacer/vrr/profilecodec.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTemporaryDir>
#include <QCryptographicHash>
#include <SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

static std::atomic_uint64_t virtualNow { 1000000 };
extern "C" uint64_t LiGetMicroseconds(void) { return virtualNow.load(); }

class VrrWorkerReplayHarness {
public:
    struct Input {
        uint64_t sequence=0, arrival=0, output=0, ready=0, receive=0, reassembled=0, decodeSubmit=0;
        uint64_t prepare=0, present=0, loss=0;
        int frame=0; uint32_t rtp=0; bool rtpValid=true, cancelled=false, completion=false;
    };
    struct Presenter : IVrrFramePresenter {
        VrrWorkerReplayHarness* h=nullptr; Input* active=nullptr; uint64_t serial=0;
        VrrFallbackReason checkSupport() const override { return VrrFallbackReason::NoFallback; }
        bool canLatchAdaptivePresent() const override { return true; }
        VrrDecodeReadiness observeDecodeReadiness(AVFrame* f,uint64_t) override {
            auto& i=h->inputs[size_t(f->pts)]; const auto start=LiGetMicroseconds();
            if (i.completion) h->advance(std::max(start,i.ready));
            return {LiGetMicroseconds()-start,i.completion,!i.completion};
        }
        VrrPrepareResult prepareFrame(AVFrame* f,uint64_t) override {
            active=&h->inputs[size_t(f->pts)]; h->advance(LiGetMicroseconds()+active->prepare);
            VrrPrepareResult r;r.prepared=true;r.sourceFrameReusable=true;return r;
        }
        VrrPresentFeedback presentAdaptive(const VrrPresentRequest&) override {
            VrrPresentFeedback f;f.presented=!active->cancelled;f.cancelled=active->cancelled;
            f.nativeBackendValid=true;f.nativeBackend=VrrNativePresentationBackend::Vulkan;
            f.nativePresentResultValid=true;f.nativePresentResult=f.presented?0:-1;
            f.submissionTimeValid=true;f.submissionTimeUs=LiGetMicroseconds();
            f.submissionIdValid=f.presented;++serial;f.submissionId=f.presented?serial:0;
            // Unknown display timing stays unknown. Never echo captured
            // display events onto a counterfactual enqueue timeline.
            h->advance(LiGetMicroseconds()+active->present);return f;
        }
    } presenter;
    std::vector<Input> inputs;
    std::unique_ptr<TrackedFrameLifetime[]> lifetime;
    std::unique_ptr<VrrPacingWorker> worker;
    PacerTelemetry telemetry;
    size_t next=0, unknownService=0, observedPreparedAhead=0;
    VrrSessionConfig config;
    VrrTimingParameters parameters;
    QByteArray inputHash;
    QByteArray initialProfile;
    bool permitMissing=false;

    bool load(const QString& path,QString& error) {
        QFile f(path);if(!f.open(QIODevice::ReadOnly)){error=f.errorString();return false;}
        const auto bytes=f.readAll();inputHash=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();
        const auto lines=bytes.split('\n');if(lines.empty()){error="empty CSV";return false;}
        const auto header=lines[0].trimmed().split(',');QMap<QByteArray,int> columns;
        uint64_t expectedRows=0;
        if(header.contains("trace_schema")) {
            const auto marker=bytes.indexOf("#vrr_trace_footer,");
            if(marker<1 || bytes[marker-1]!='\n' || bytes.indexOf("#vrr_trace_footer,",marker+1)>=0) {
                error="capture lacks one complete clean-close footer";return false;
            }
            const auto tail=bytes.mid(marker).trimmed();
            if(tail.contains('\n')) {error="data after trace footer";return false;}
            QMap<QByteArray,QByteArray> footer;
            for(const auto& part:tail.split(',')) {
                const auto eq=part.indexOf('=');if(eq>0)footer[part.left(eq)]=part.mid(eq+1);
            }
            if(footer.value("clean_shutdown")!="1" || footer.value("rows_dropped")!="0" ||
               footer.value("write_failed")!="0" || footer.value("size_capped")!="0") {
                error="capture incomplete/dropped/capped/write-failed";return false;
            }
            if(QCryptographicHash::hash(bytes.left(marker),QCryptographicHash::Sha256).toHex()!=footer.value("decoded_sha256")) {
                error="capture decoded checksum mismatch";return false;
            }
            bool ok=false;expectedRows=footer.value("rows_enqueued").toULongLong(&ok);
            if(!ok || !expectedRows) {error="invalid footer row count";return false;}
        }
        for(int k=0;k<header.size();++k)columns[header[k]]=k;
        for(const auto& key:{"arrival_sequence","frame","rtp_timestamp","rtp_valid","pacer_arrival_us","decoder_output_us"})
            if(!columns.contains(key)){error=QString("missing input column: ")+key;return false;}
        bool session=false;QJsonObject snapshot;
        for(int line=1;line<lines.size();++line){
            if(lines[line].trimmed().isEmpty()||lines[line].startsWith('#'))continue;
            const auto v=lines[line].trimmed().split(',');if(v.size()!=header.size()){error="malformed CSV";return false;}
            const auto value=[&](const char* key)->uint64_t {const int n=columns.value(key,-1);return n<0?0:v[n].toULongLong();};
            Input i;i.sequence=value("arrival_sequence");i.frame=int(value("frame"));i.rtp=uint32_t(value("rtp_timestamp"));i.rtpValid=value("rtp_valid");
            i.arrival=value("pacer_arrival_us");i.output=value("decoder_output_us");i.ready=value("decode_complete_us");
            i.receive=value("frame_receive_us");i.reassembled=value("frame_reassembled_us");i.decodeSubmit=value("decode_submit_us");
            i.prepare=value("prepare_us");i.present=value("present_call_us");i.cancelled=value("cancelled");i.loss=value("lost_packets");
            i.completion=value("decode_readiness_observed")||value("model_decode_observed");
            observedPreparedAhead+=value("prepared_ahead")!=0;
            if(!i.prepare){++unknownService;i.prepare=400;}
            if(!i.present)i.present=20;
            if(!i.sequence||!i.arrival||!i.output||i.output>i.arrival){error="invalid immutable arrival/output metadata";return false;}
            inputs.push_back(i);
            if(!session){
                if(value("param_native_synchronized_presentation")||value("param_native_scheduled_presentation")){error="only Deck non-native-sync/non-scheduled path supported";return false;}
                if(columns.contains("can_latch_present")&&!value("can_latch_present")){error="Deck latch-capable path required";return false;}
                config.displayRefreshHz=int(value("display_refresh_hz"));config.streamRateHz=int(value("stream_rate_hz"));
                config.latencyMode=int(value("session_latency_mode"));config.smoothFrameTiming=true;
                config.allowAdditionalQueuedFrame=value("additional_queued_frame");
                config.experimentalInputPercentilePpm=value("param_playout_input_percentile_target_per_million");
                config.experimentalInputPercentileWindowUs=value("param_playout_input_percentile_window_us");
                if(columns.contains("playout_initial_profile"))initialProfile=v[columns["playout_initial_profile"]];
#define VRR_READ_REPLAY_PARAMETER(type,name,member,defaultValue) \
                if(columns.contains("param_" #name)) snapshot[#name]=QString::fromLatin1(v[columns["param_" #name]]).toDouble();
                VRR_TIMING_PARAMETER_FIELDS(VRR_READ_REPLAY_PARAMETER)
#undef VRR_READ_REPLAY_PARAMETER
                session=true;
            }
        }
        if(expectedRows && expectedRows!=inputs.size()) {error="footer row count mismatch";return false;}
        if(inputs.empty()||config.displayRefreshHz<=0||config.streamRateHz<=0){error="missing session/input";return false;}
        if(observedPreparedAhead){error="prepared-ahead renderer lifecycle is not modeled";return false;}
        if(unknownService&&!permitMissing){error="unmeasured dropped-frame service costs: explicit --allow-missing-costs required (400us prepare/20us present assumption)";return false;}
        std::sort(inputs.begin(),inputs.end(),[](const Input&a,const Input&b){return a.arrival<b.arrival||(a.arrival==b.arrival&&a.sequence<b.sequence);});
        std::vector<uint64_t> sequences;for(auto&i:inputs)sequences.push_back(i.sequence);
        std::sort(sequences.begin(),sequences.end());
        for(size_t k=0;k<sequences.size();++k)if(sequences[k]!=k+1){error="incomplete/duplicate arrival sequence";return false;}
        // Missing fields select their historical schema defaults, exactly as
        // frozen-input replay does. Never silently import today's resolver.
        parameters=VrrTimingParameters{};
        return applyVrrReplayControllerSnapshot(snapshot,parameters,error);
    }
    void advance(uint64_t until) {
        until=std::max(until,LiGetMicroseconds());
        while(next<inputs.size()&&inputs[next].arrival<=until){
            const size_t k=next++;const auto&i=inputs[k];virtualNow.store(std::max(LiGetMicroseconds(),i.arrival));
            auto f=makeTrackedPacedFrame(i.frame,i.rtp,i.output,lifetime[k]);
            f.frame()->pts=int64_t(k);f.setDeliveryTimeline(i.receive,i.reassembled,i.decodeSubmit);f.setLostPackets(uint32_t(i.loss));
            f.setDecoderOutputComplete(i.completion&&i.ready<=i.output);
            worker->submit(std::move(f));
        }
        virtualNow.store(std::max(until,LiGetMicroseconds()));
    }
    static void before(void* context,bool reserved) {
        auto& h=*static_cast<VrrWorkerReplayHarness*>(context);auto& w=*h.worker;
        while(!w.replayStartReady.load())SDL_Delay(1);
        // Offline-only disk throttling never advances virtual time. Production
        // tracing remains nonblocking and drops/accounting remain unchanged.
        while(w.m_TraceRowsEnqueued.load()>w.replayTraceConsumed.load()+2048) SDL_Delay(1);
        if(reserved||w.m_FrameQueueDepth.load())return;
        if(h.next<h.inputs.size())h.advance(std::max(LiGetMicroseconds(),h.inputs[h.next].arrival));
        else w.m_Stopping.store(true);
    }
    bool run(QString trace,bool audit,QString& error) {
        presenter.h=this;lifetime=std::make_unique<TrackedFrameLifetime[]>(inputs.size());
        virtualNow.store(inputs.front().arrival);
        qputenv("MOONLIGHT_VRR_TRACE",QFile::encodeName(trace));qputenv("MOONLIGHT_VRR_BUFFER_AUDIT",audit?"1":"0");
        config.calibrationKey.clear();config.calibrationPath.clear();
        worker=std::make_unique<VrrPacingWorker>(&presenter,config,&telemetry);
        worker->m_TimingController=std::make_unique<VrrTimingController>(worker->m_Config,true,parameters);
        if(!initialProfile.isEmpty()) {
            std::vector<int64_t> words;
            if(!decodeVrrPlayoutProfile(initialProfile,words)||!worker->m_TimingController->loadPlayoutHistory(words)) {
                error="captured startup history incompatible with selected controller";return false;
            }
        }
        worker->m_QueueCapacity=worker->m_TimingController->queuedFrameCapacity();
        worker->replayBeforeDequeue=before;worker->replayContext=this;
        VrrTargetWaiterHooks hooks;hooks.nowUs=[] {return LiGetMicroseconds();};
        hooks.sleepForUs=[this](uint64_t n){advance(LiGetMicroseconds()+n);};
        hooks.yield=[this]{advance(LiGetMicroseconds()+10);};
        worker->m_TargetWaiter=std::make_unique<VrrTargetWaiter>(hooks);
        if(worker->m_NativeSynchronizedPresentation||worker->m_NativeScheduledPresentation){error="Deck native-sync contract mismatch";return false;}
        if(!worker->start()){error="worker start failed";return false;}
        SDL_WaitThread(worker->m_WorkerThread,nullptr);worker->m_WorkerThread=nullptr;
        worker.reset();
        for(size_t k=0;k<inputs.size();++k)if(lifetime[k].releases!=1){error="frame ownership/release mismatch";return false;}
        return true;
    }
};

int main(int argc,char**argv) {
    QCoreApplication app(argc,argv);QCommandLineParser p;p.addHelpOption();
    p.setApplicationDescription("Production-worker counterfactual simulation. NOT frozen-input exact replay or physical display proof.");
    p.addPositionalArgument("input.csv","Complete expanded trace or declared synthetic arrival/service CSV.");
    p.addOption({"trace","New trace output (CSV or .vrrtrace).","path"});
    p.addOption({"output","Fidelity/parameters JSON output.","path"});
    p.addOption({"lifecycle","captured,0,1,2; no other policy changes.","mode","captured"});
    p.addOption({"audit","Opt-in scalar snapshots,0 or1.","enabled","1"});
    p.addOption({"allow-missing-costs","Acknowledge400us preparation/20us present assumptions for unmeasured dropped frames."});
    p.addOption({"audit-benchmark","Measure scalar snapshot batch cost only; requires --output, no input/GPU/client."});
    p.process(app);
    if(p.isSet("audit-benchmark")) {
        if(!p.isSet("output"))p.showHelp(2);
        VrrSessionConfig c;c.displayRefreshHz=120;c.streamRateHz=116;
        VrrTimingController controller(c,true,vrrTimingParametersForSession(c,false,false));
        std::array<double,128> samples;volatile uint64_t checksum=0;
        for(auto& sample:samples) {
            const auto begin=std::chrono::steady_clock::now();
            for(unsigned n=0;n<1024;++n) {const auto s=controller.bufferAudit(1000000+n,6000);checksum=checksum+s.appliedUs+s.flags;}
            sample=double(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-begin).count())/1024;
        }
        std::sort(samples.begin(),samples.end());double sum=0;for(auto x:samples)sum+=x;
        QJsonObject j{{"scope","128 batch means of1024 scalar snapshots; empty scalar stats; not per-frame/live trace overhead"},
            {"unit","ns per snapshot, averaged within each batch"},{"n",128},{"min",samples[0]},{"mean",sum/128},
            {"p50",samples[63]},{"p95",samples[121]},{"p99",samples[126]},{"max",samples[127]},
            {"snapshot_bytes",int(sizeof(VrrBufferAuditSnapshot))},{"ring_snapshot_payload_bytes",qint64(2*sizeof(VrrBufferAuditSnapshot)*8192)},
            {"checksum",qint64(checksum)}};
        QFile file(p.value("output"));if(!file.open(QIODevice::WriteOnly))return 1;file.write(QJsonDocument(j).toJson());return 0;
    }
    if(p.positionalArguments().size()!=1||!p.isSet("trace")||!p.isSet("output"))p.showHelp(2);
    QTemporaryDir privateSettings;qputenv("XDG_CONFIG_HOME",QFile::encodeName(privateSettings.path()));
    qputenv("XDG_CACHE_HOME",QFile::encodeName(privateSettings.path()));qputenv("XDG_DATA_HOME",QFile::encodeName(privateSettings.path()));
    if(SDL_Init(SDL_INIT_TIMER)!=0)return 2;
    VrrWorkerReplayHarness h;h.permitMissing=p.isSet("allow-missing-costs");QString error;
    if(!h.load(p.positionalArguments()[0],error)){std::fprintf(stderr,"%s\n",qPrintable(error));return 2;}
    if(p.value("lifecycle")!="captured"){
        bool ok=false;const uint64_t m=p.value("lifecycle").toULongLong(&ok);if(!ok||m>2)return 2;h.parameters.playoutInputLayerLifecycle=m;
    }
    if(!validateVrrTimingParameters(h.parameters,error)){std::fprintf(stderr,"%s\n",qPrintable(error));return 2;}
    const auto start=std::chrono::steady_clock::now();
    if(!h.run(p.value("trace"),p.value("audit")=="1",error)){std::fprintf(stderr,"%s\n",qPrintable(error));return 1;}
    QJsonObject result{{"model","production-worker-closed-loop-v1"},{"frozen_input_exact_replay",false},{"physical_display_proof",false},
        {"all_arrivals_readmitted",true},{"production_queue_drop_schedule_feedback",true},{"native_sync",false},{"native_scheduled",false},
        {"display_observation","unavailable; no captured display events transplanted"},{"source_input_sha256",QString::fromLatin1(h.inputHash)},
        {"inputs",qint64(h.inputs.size())},{"unmeasured_preparation_cost_frames",qint64(h.unknownService)},
        {"service_scope","recorded per-frame CPU costs/completion bounds, external cancellations reused; no GPU/resource contention or decoder backpressure prediction"},
        {"runtime_ms",double(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count())/1000},
        {"controller",vrrTimingParametersToJson(h.parameters)}};
    QFile out(p.value("output"));if(!out.open(QIODevice::WriteOnly))return 1;out.write(QJsonDocument(result).toJson());
    SDL_Quit();return 0;
}
