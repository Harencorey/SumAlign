#pragma once
//==============================================================================
// SumAlign - the session hub (Phase 3): every SumAlign instance in the DAW process registers here.
// Plain C++ (no JUCE), so it can move into The Gate later.
//
//   audio threads:  push (member, host timeline position, mono samples)  ->  per-member lock-free rings
//   hub thread:     drain rings -> line all tracks up by timeline position -> NetworkEstimator (in sync)
//                   -> every ~2 s: pair estimates -> solveSession -> results per member
//   message threads: start / finish / cancel, REF, names, read results (each instance applies its own)
//
// Tracks are lined up by the host's timeline position, so SumAlign should sit first in each track's
// chain (a plug-in with latency before it would shift that track). The hub lives as long as at least one
// instance holds it; the last one to leave stops its thread.
//==============================================================================

#include "AlignNetwork.h"
#include "AlignCore.h"

#include <map>
#include <memory>
#include <string>

namespace sa
{

class AlignHub
{
public:
    enum class State { idle, listening, done };

    struct MemberView
    {
        int id = 0;
        std::string name;
        bool ref = false;
        int group = -1;           // the user's setting: -1 Auto, -2 Off, 0..7 = A..H
        Family family = Family::unknown;   // from the name
        bool nameSaysBus = false; // the name looks like a bus/effect: an Auto instance sits out
        bool off() const { return group == -2 || (group == -1 && nameSaysBus); }
        bool live = false;        // has sent audio during this ALIGN
        int hits = 0;
        TrackAlign result;        // valid when the hub's state is done (or as a preview while listening)
    };

    struct Status
    {
        State state = State::idle;
        double progress = 0.0;
        double heardSeconds = 0.0;
        bool playing = false;     // audio arrived recently
        int groups = 0;
        std::vector<MemberView> members;
        int id = 0;               // increments with every finished ALIGN
    };

    static constexpr double kMinSeconds   = 20.0;   // audio before it may finish on its own
    static constexpr double kMaxSeconds   = 180.0;
    static constexpr int    kStableEvals  = 3;      // unchanged results this many times in a row (~2 s apart)

    // one hub per process
    static std::shared_ptr<AlignHub> get()
    {
        static std::mutex m;
        static std::weak_ptr<AlignHub> weak;
        std::lock_guard<std::mutex> lk (m);
        auto s = weak.lock();
        if (! s) { s = std::shared_ptr<AlignHub> (new AlignHub()); weak = s; }
        return s;
    }

    ~AlignHub()
    {
        {
            std::lock_guard<std::mutex> lk (mtx);
            quit = true;
            cv.notify_all();
        }
        if (worker.joinable()) worker.join();
    }

    //--- members (message thread) ----------------------------------------------------------
    // returns 0 when the session is full (kMaxMembers)
    int join (const std::string& name)
    {
        std::lock_guard<std::mutex> lk (mtx);
        for (auto& m : pool)
        {
            if (m.used.load()) continue;
            if (! m.allocated) { m.ring.prepare (1 << 18); m.allocated = true; }   // ~5 s at 48 kHz of slack
            m.ring.clear();
            m.id = nextId++;
            m.name = name;
            m.group = -1;
            m.local.clear(); m.hasData = false;
            m.used.store (true, std::memory_order_release);
            return m.id;
        }
        return 0;
    }

    void leave (int id)
    {
        std::unique_lock<std::mutex> lk (mtx);
        cv.wait (lk, [this] { return ! busy; });
        if (auto* m = find (id)) m->used.store (false, std::memory_order_release);
        if (refId == id) refId = -1;
    }

    void setName (int id, const std::string& name)  { std::lock_guard<std::mutex> lk (mtx); if (auto* m = find (id)) m->name = name; }
    // -1 = Auto, -2 = Off, 0..7 = manual group A..H
    void setGroup (int id, int g)                   { std::lock_guard<std::mutex> lk (mtx); if (auto* m = find (id)) m->group = std::clamp (g, -2, 7); }
    void setRef (int id, bool on)                   { std::lock_guard<std::mutex> lk (mtx); refId = on ? id : (refId == id ? -1 : refId); }
    int  numMembers() const                         { std::lock_guard<std::mutex> lk (mtx); int c = 0; for (auto& m : pool) c += m.used.load() ? 1 : 0; return c; }

    //--- audio thread ------------------------------------------------------------------------
    bool isListening() const noexcept { return listening.load (std::memory_order_acquire); }

    // timelinePos: the host's sample position of mono[0]. Never blocks or allocates.
    void push (int id, long long timelinePos, const float* mono, int n, double sampleRate) noexcept
    {
        if (! isListening() || sampleRate != hubRate.load (std::memory_order_relaxed)) return;
        // the pool never moves or shrinks, so this lookup needs no lock
        for (auto& m : pool)
            if (m.used.load (std::memory_order_acquire) && m.id == id) { m.ring.push (timelinePos, mono, n); return; }
    }

    //--- ALIGN (message thread) ---------------------------------------------------------------
    void start (double sampleRate, int maxLagSamples)
    {
        std::unique_lock<std::mutex> lk (mtx);
        if (! worker.joinable()) worker = std::thread ([this] { loop(); });
        listening.store (false);
        cv.wait (lk, [this] { return ! busy; });
        sr = sampleRate;
        maxLag = maxLagSamples;
        hubRate.store (sampleRate);
        slots.clear();
        for (auto& m : pool)
            if (m.used.load()) { m.ring.clear(); m.local.clear(); m.localStart = 0; m.hasData = false; m.lastSeen = 0; slots.push_back (m.id); }
        est.prepare (sr, maxLag, (int) slots.size());
        fed = -1; heard = 0; stable = 0; quiet = 0; lastResult.clear();
        status.state = State::listening;
        status.progress = 0.0;
        status.heardSeconds = 0.0;
        status.groups = 0;
        results.assign (slots.size(), TrackAlign {});
        stopReq = false;
        listening.store (true, std::memory_order_release);
        cv.notify_all();
    }

    void finishNow() { std::lock_guard<std::mutex> lk (mtx); if (status.state == State::listening) stopReq = true; cv.notify_all(); }

    void cancel()
    {
        listening.store (false);
        std::lock_guard<std::mutex> lk (mtx);
        status.state = State::idle;
    }

    Status getStatus() const
    {
        std::lock_guard<std::mutex> lk (mtx);
        Status s = status;
        s.members.clear();
        for (auto& mm : pool)
        {
            if (! mm.used.load()) continue;
            const Member* m = &mm;
            MemberView v;
            v.id = m->id; v.name = m->name; v.ref = refId == m->id; v.group = m->group;
            const auto ni = classifyTrackName (m->name);
            v.family = ni.family; v.nameSaysBus = ni.bus;
            for (size_t k = 0; k < slots.size(); ++k)
                if (slots[k] == m->id)
                {
                    v.live = m->hasData;
                    v.hits = k < (size_t) est.numTracks() ? est.hitsOf ((int) k) : 0;
                    if (k < results.size()) v.result = results[k];
                }
            if (v.off() && v.result.group < 0) v.result.why = LeftAlone::off;
            s.members.push_back (v);
        }
        return s;
    }

    // unthreaded use (tests, tools): run one hub pass now
    void pumpForTests() { std::unique_lock<std::mutex> lk (mtx); passLocked (lk); }

private:
    AlignHub() = default;

    // ring of (timeline position, sample) runs: one SPSC float ring + a ring of run headers
    struct TimedRing
    {
        struct Run { long long t0; int len; };
        void prepare (int cap)
        {
            samples.prepare (cap);
            runs.assign (4096, Run {});
            rw.store (0); rr.store (0);
        }
        void clear() { samples.clear(); rw.store (0); rr.store (0); }
        void push (long long t0, const float* x, int n) noexcept
        {
            const size_t w = rw.load (std::memory_order_relaxed), r = rr.load (std::memory_order_acquire);
            if (w - r >= runs.size()) return;                       // header ring full: drop
            const int before = samples.dropped.load();
            samples.push (x, x, n);                                 // CaptureRing stores pairs; we use one side
            const int lost = samples.dropped.load() - before;
            if (lost >= n) return;
            runs[w % runs.size()] = { t0, n - lost };
            rw.store (w + 1, std::memory_order_release);
        }
        bool pop (Run& run, std::vector<float>& out, std::vector<float>& scratch)
        {
            const size_t r = rr.load (std::memory_order_relaxed), w = rw.load (std::memory_order_acquire);
            if (r == w) return false;
            run = runs[r % runs.size()];
            out.resize ((size_t) run.len); scratch.resize ((size_t) run.len);
            int got = 0;
            while (got < run.len) { const int k = samples.pop (out.data() + got, scratch.data() + got, run.len - got); if (k <= 0) break; got += k; }
            run.len = got;
            rr.store (r + 1, std::memory_order_release);
            return true;
        }
        CaptureRing samples;
        std::vector<Run> runs;
        std::atomic<size_t> rw { 0 }, rr { 0 };
    };

    struct Member
    {
        std::atomic<bool> used { false };
        bool allocated = false;
        int id = 0;
        std::string name;
        int group = -1;
        TimedRing ring;
        std::vector<float> local;     // contiguous audio starting at localStart (timeline samples)
        long long localStart = 0;
        bool hasData = false;
        long long lastSeen = 0;       // timeline end of the latest audio
    };

    Member* find (int id) { for (auto& m : pool) if (m.used.load() && m.id == id) return &m; return nullptr; }

    void loop()
    {
        std::unique_lock<std::mutex> lk (mtx);
        while (! quit)
        {
            cv.wait_for (lk, std::chrono::milliseconds (50));
            if (quit) break;
            passLocked (lk);
        }
    }

    void passLocked (std::unique_lock<std::mutex>& lk)
    {
        if (status.state != State::listening || busy) return;
        busy = true;
        const bool stop = stopReq; stopReq = false;
        const int refSnap = refId;
        std::vector<Member*> live;
        for (auto& m : pool) if (m.used.load()) live.push_back (&m);
        SessionHints hints;
        hints.group.assign (slots.size(), -2);        // a slot whose instance has gone: off
        hints.family.assign (slots.size(), Family::unknown);
        for (size_t k = 0; k < slots.size(); ++k)
            for (auto* m : live)
                if (m->id == slots[k])
                {
                    const auto ni = classifyTrackName (m->name);
                    hints.group[k] = (m->group == -1 && ni.bus) ? -2 : m->group;
                    hints.family[k] = ni.family;
                }
        lk.unlock();

        // 1) drain each member's ring into its contiguous local buffer
        bool any = false;
        std::vector<float> tmp, scratch;
        for (auto* m : live)
        {
            TimedRing::Run run;
            while (m->ring.pop (run, tmp, scratch))
            {
                if (run.len <= 0) continue;
                any = true;
                if (! m->hasData || run.t0 != m->localStart + (long long) m->local.size())
                {
                    // first audio, or a jump/gap: start this member's run again
                    m->local.assign (tmp.begin(), tmp.begin() + run.len);
                    m->localStart = run.t0;
                    m->hasData = true;
                }
                else m->local.insert (m->local.end(), tmp.begin(), tmp.begin() + run.len);
                m->lastSeen = run.t0 + run.len;
            }
        }
        if (any) quiet = 0; else ++quiet;

        // 2) the range every live member covers; feed it to the estimator in sync
        long long newest = 0;
        for (auto* m : live) if (m->hasData) newest = std::max (newest, m->lastSeen);
        long long from = -1, to = (1LL << 62);
        std::vector<Member*> bySlot (slots.size(), nullptr);
        for (size_t k = 0; k < slots.size(); ++k)
            for (auto* m : live) if (m->id == slots[k]) bySlot[k] = m;
        for (size_t k = 0; k < bySlot.size(); ++k)
        {
            auto* m = bySlot[k];
            if (m == nullptr || hints.off ((int) k) || ! m->hasData || m->lastSeen < newest - (long long) (1.5 * sr)) continue;   // not live / sitting out
            from = std::max (from, m->localStart);
            to = std::min (to, m->localStart + (long long) m->local.size());
        }
        if (from >= 0 && to > from)
        {
            if (fed >= 0 && from != fed) est.breakStream();   // transport jumped / a gap
            const int count = (int) (to - from);
            std::vector<const float*> ptr (slots.size(), nullptr);
            for (size_t k = 0; k < slots.size(); ++k)
            {
                auto* m = bySlot[k];
                if (m != nullptr && ! hints.off ((int) k) && m->hasData && m->localStart <= from && m->localStart + (long long) m->local.size() >= to)
                    ptr[k] = m->local.data() + (from - m->localStart);
            }
            for (int pos = 0; pos < count; pos += 4096)
            {
                const int n = std::min (4096, count - pos);
                est.feed (ptr.data(), n);
                for (auto& p : ptr) if (p != nullptr) p += n;
            }
            heard += count;
            fed = to;
            // drop what's been fed
            for (auto* m : bySlot)
                if (m != nullptr && m->hasData && to > m->localStart)
                {
                    const long long cut = std::min<long long> (to - m->localStart, (long long) m->local.size());
                    m->local.erase (m->local.begin(), m->local.begin() + cut);
                    m->localStart += cut;
                }
        }
        // members that keep sending but are never in range (e.g. far behind) don't block forever: trim to 10 s
        for (auto* m : live)
            if (m->local.size() > (size_t) (10.0 * sr))
            {
                const size_t cut = m->local.size() - (size_t) (10.0 * sr);
                m->local.erase (m->local.begin(), m->local.begin() + (long long) cut);
                m->localStart += (long long) cut;
            }

        // 3) every ~2 s of new audio (or when asked to stop): solve
        std::vector<TrackAlign> res;
        const bool evaluate = stop || heard - lastEval >= (long long) (2.0 * sr) || (quiet == 30 && heard > lastEval);
        if (evaluate)
        {
            lastEval = heard;
            const auto edges = est.directedEdges();
            int ref = -1;
            for (size_t k = 0; k < slots.size(); ++k) if (slots[k] == refSnap) ref = (int) k;
            res = solveSession ((int) slots.size(), edges, ref, (double) maxLag, hints);
            for (size_t k = 0; k < res.size(); ++k)
                if (res[k].group < 0 && res[k].why != LeftAlone::off && (bySlot[k] == nullptr || ! bySlot[k]->hasData))
                    res[k].why = LeftAlone::noAudio;
        }

        lk.lock();
        busy = false;
        cv.notify_all();
        if (status.state != State::listening) return;
        status.heardSeconds = (double) heard / sr;
        status.playing = quiet < 30;   // audio within the last ~1.5 s
        if (evaluate)
        {
            bool same = res.size() == lastResult.size();
            for (size_t k = 0; same && k < res.size(); ++k)
                same = res[k].group == lastResult[k].group && res[k].letter == lastResult[k].letter && res[k].polarity == lastResult[k].polarity
                    && std::abs (res[k].delaySamples - lastResult[k].delaySamples) <= 0.5;
            stable = same ? stable + 1 : 0;
            lastResult = res;
            results = res;
            int g = -1; for (auto& r : res) g = std::max (g, r.group);
            status.groups = g + 1;
        }
        status.progress = std::max (status.progress, std::min (0.99, 0.7 * status.heardSeconds / kMinSeconds + 0.1 * stable));
        const bool enough = status.heardSeconds >= kMinSeconds && status.groups > 0;
        const bool settled = enough && stable >= kStableEvals;
        const bool stoppedPlaying = quiet >= 40 && status.heardSeconds >= 0.5 * kMinSeconds && status.groups > 0;
        if (settled || stop || stoppedPlaying || status.heardSeconds >= kMaxSeconds)
        {
            listening.store (false, std::memory_order_release);
            status.state = State::done;
            status.progress = 1.0;
            ++status.id;
        }
    }

    mutable std::mutex mtx;
    std::condition_variable cv;
    std::thread worker;
    bool quit = false, busy = false, stopReq = false;
    std::atomic<bool> listening { false };
    std::atomic<double> hubRate { 0.0 };

    static constexpr int kMaxMembers = 64;
    std::array<Member, kMaxMembers> pool;
    int nextId = 1, refId = -1;

    double sr = 48000.0;
    int maxLag = 1440;
    std::vector<int> slots;            // member id per estimator track, fixed for one ALIGN
    NetworkEstimator est;
    long long fed = -1, heard = 0, lastEval = 0;
    int stable = 0, quiet = 0;
    std::vector<TrackAlign> results, lastResult;
    Status status;
};

} // namespace sa
