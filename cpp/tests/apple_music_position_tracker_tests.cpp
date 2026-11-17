#include "applemusic/AppleMusicPositionTracker.h"

#include <atomic>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

namespace events = smtc::applemusic::property_events;
using smtc::applemusic::AppleMusicPositionTracker;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void testEventTimeIsReused() {
    AppleMusicPositionTracker tracker;
    auto sample = tracker.query(1, 420000000, 1000, true);
    require(!sample.fromEvent && sample.queryQpc == 1000, "the first query is only an approximate sample");
    require(tracker.event(1, 430000000, 2000, true), "a position event should be accepted for the tracked item");
    sample = tracker.query(1, 430000000, 2450, true);
    require(sample.fromEvent && sample.position100ns == 430000000 && sample.queryQpc == 2000,
            "a query must preserve the position event's original QPC timestamp");
    sample = tracker.query(1, 430000000, 2999, true);
    require(sample.fromEvent && sample.queryQpc == 2000, "repeated snapshots must not restamp a cached position");
}

void testFallbackDoesNotPretendToUpdate() {
    AppleMusicPositionTracker tracker;
    tracker.query(1, 100000000, 1000, true);
    auto sample = tracker.query(1, 100000000, 1900, true);
    require(!sample.fromEvent && sample.queryQpc == 1000, "unchanged fallback positions must keep their first observation time");
    sample = tracker.query(1, 110000000, 2100, true);
    require(!sample.fromEvent && sample.queryQpc == 2100, "changed query-only positions must remain explicitly approximate");
}

void testPauseAndResumeNeverReusePauseAge() {
    AppleMusicPositionTracker tracker;
    tracker.query(1, 420000000, 1000, true);
    tracker.event(1, 430000000, 2000, true);
    auto sample = tracker.query(1, 431000000, 2600, false);
    require(!sample.playing && sample.position100ns == 431000000 && sample.queryQpc == 2600,
            "pause must use the exact current raw position without extrapolation");
    sample = tracker.query(1, 431000000, 9000, true);
    require(sample.playing && !sample.fromEvent && sample.queryQpc == 9000,
            "resume at the same position must not extrapolate across the paused interval");
    tracker.query(1, 431000000, 9500, false);
    tracker.event(1, 435000000, 12000, true);
    sample = tracker.query(1, 435000000, 12300, true);
    require(sample.fromEvent && sample.queryQpc == 12000,
            "a real resume event before the next query must retain its accurate timestamp");
}

void testTrackChangesRejectOldCallbacks() {
    AppleMusicPositionTracker tracker;
    tracker.query(1, 900000000, 1000, true);
    tracker.event(1, 910000000, 2000, true);
    tracker.track(2, 2500);
    require(!tracker.event(1, 920000000, 2600, true), "late callbacks from the previous track must be ignored");
    require(tracker.event(2, 10000000, 2700, true), "new track events must be accepted");
    auto sample = tracker.query(2, 10000000, 2900, true);
    require(sample.fromEvent && sample.position100ns == 10000000, "the old track position must not survive a track change");
    require(tracker.event(3, 20000000, 3000, true, true), "player replacement events may establish the next item before polling");
    sample = tracker.query(3, 20000000, 3300, true);
    require(sample.fromEvent && sample.queryQpc == 3000, "polling a replacement must preserve the captured replacement event");
    require(!tracker.event(3, 0, 2900, true), "out-of-order events must not replace a newer position");
}

void testMissedEventFallsBackSafely() {
    AppleMusicPositionTracker tracker;
    tracker.query(1, 100000000, 1000, true);
    tracker.event(1, 110000000, 2000, true);
    const auto sample = tracker.query(1, 900000000, 2500, true);
    require(!sample.fromEvent && sample.position100ns == 900000000 && sample.queryQpc == 2500,
            "a raw seek without a matching event must not continue extrapolating the old event");
}

void testDelayedSnapshotCannotRestorePreviousIdentity() {
    AppleMusicPositionTracker tracker;
    tracker.query(1, 900000000, 100, true);
    tracker.track(2, 201);
    tracker.event(2, 10000000, 202, true);
    require(!tracker.track(1, 200), "a late observation of the previous item must not reverse the tracked identity");
    const auto oldSample = tracker.query(1, 910000000, 260, true, 200);
    require(!oldSample.fromEvent && oldSample.position100ns == 910000000,
            "an old snapshot must return its own approximate sample without changing tracked state");
    const auto current = tracker.query(2, 10000000, 300, true);
    require(current.fromEvent && current.queryQpc == 202 && tracker.identity() == 2,
            "the new item's accurate sample must survive an older snapshot processed later");
}

void testDuplicateNotificationsDoNotRestampPosition() {
    AppleMusicPositionTracker tracker;
    tracker.query(1, 100000000, 1000, true);
    require(!tracker.event(1, 100000000, 1200, true),
            "an unchanged cached value must not become precise merely because it was notified again");
    auto sample = tracker.query(1, 100000000, 1400, true);
    require(!sample.fromEvent && sample.queryQpc == 1000,
            "a repeated notification must retain approximate position age");
    require(tracker.event(1, 110000000, 2000, true), "a changed position notification should be precise");
    tracker.event(1, 110000000, 2300, true);
    sample = tracker.query(1, 110000000, 2400, true);
    require(sample.fromEvent && sample.queryQpc == 2000,
            "duplicate position or all-property events must preserve the original update timestamp");
}

void testQueryBeforeNotificationPreservesRealEventTime() {
    AppleMusicPositionTracker tracker;
    tracker.query(1, 100000000, 1000, true);
    tracker.event(1, 100000000, 1100, true);
    tracker.query(1, 110000000, 2050, true);
    require(tracker.event(1, 110000000, 2000, true),
            "a real changed-position event must survive a query that already read the new value");
    auto sample = tracker.query(1, 110000000, 2200, true);
    require(sample.fromEvent && sample.queryQpc == 2000,
            "the earlier publishing timestamp must replace the later query-only observation time");
    tracker.event(1, 110000000, 2300, true);
    sample = tracker.query(1, 110000000, 2400, true);
    require(sample.queryQpc == 2000, "a subsequent duplicate callback must still preserve publishing time");
}

struct HandlerVTable : events::UnknownVTable {
    HRESULT (__stdcall* Invoke)(void*, void*, void*) noexcept;
};

struct FakeObservable {
    events::NotifyVTable* vtable = nullptr;
    std::atomic<ULONG> references{1};
    std::mutex mutex;
    void* handler = nullptr;
    void* delayedReference = nullptr;
    bool supportsNotify = true;
    bool legacyOnly = false;
    bool failAdd = false;
    bool failRemove = false;
    bool retainAfterRemove = false;
    bool fireDuringAdd = false;
    bool invokeDuringRemove = false;
    int additions = 0;
    int removals = 0;
    std::int64_t position100ns = 0;

    FakeObservable() : vtable(&kVTable) {}

    void fire() {
        void* current = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            current = handler;
            if (current) events::table<events::UnknownVTable>(current)->AddRef(current);
        }
        if (current) {
            events::table<HandlerVTable>(current)->Invoke(current, this, nullptr);
            events::release(current);
        }
    }

    void releaseDelayedReference() {
        events::release(delayedReference);
        delayedReference = nullptr;
    }

    static HRESULT __stdcall query(void* raw, REFIID iid, void** result) noexcept {
        auto* self = static_cast<FakeObservable*>(raw);
        if (!result) return E_POINTER;
        *result = nullptr;
        if (IsEqualGUID(iid, events::kUnknownGuid) ||
            (self->supportsNotify && ((!self->legacyOnly && IsEqualGUID(iid, events::kNotifyGuid)) ||
                                     (self->legacyOnly && IsEqualGUID(iid, events::kLegacyNotifyGuid))))) {
            *result = raw;
            add(raw);
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    static ULONG __stdcall add(void* raw) noexcept {
        return static_cast<FakeObservable*>(raw)->references.fetch_add(1) + 1;
    }
    static ULONG __stdcall release(void* raw) noexcept {
        return static_cast<FakeObservable*>(raw)->references.fetch_sub(1) - 1;
    }
    static HRESULT __stdcall getIids(void*, ULONG*, IID**) noexcept { return E_NOTIMPL; }
    static HRESULT __stdcall getName(void*, HSTRING*) noexcept { return E_NOTIMPL; }
    static HRESULT __stdcall getTrust(void*, std::int32_t*) noexcept { return E_NOTIMPL; }
    static HRESULT __stdcall subscribe(void* raw, void* delegate, std::int64_t* token) noexcept {
        auto* self = static_cast<FakeObservable*>(raw);
        ++self->additions;
        if (self->failAdd) return E_FAIL;
        // Checking Invoke in the fourth slot also catches the mistaken
        // IInspectable-based delegate ABI that would put it in the seventh.
        void* queried = nullptr;
        auto* unknown = events::table<events::UnknownVTable>(delegate);
        const auto guid = self->legacyOnly ? events::kLegacyHandlerGuid : events::kHandlerGuid;
        if (FAILED(unknown->QueryInterface(delegate, guid, &queried))) return E_NOINTERFACE;
        events::release(queried);
        if (FAILED(unknown->QueryInterface(delegate, events::kAgileGuid, &queried))) return E_NOINTERFACE;
        events::release(queried);
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            unknown->AddRef(delegate);
            self->handler = delegate;
        }
        *token = 17;
        if (self->fireDuringAdd) {
            events::table<HandlerVTable>(delegate)->Invoke(delegate, self, nullptr);
        }
        return S_OK;
    }
    static HRESULT __stdcall unsubscribe(void* raw, std::int64_t token) noexcept {
        auto* self = static_cast<FakeObservable*>(raw);
        ++self->removals;
        if (self->failRemove || token != 17) return E_FAIL;
        void* removed = nullptr;
        {
            std::lock_guard<std::mutex> lock(self->mutex);
            removed = self->handler;
            self->handler = nullptr;
            if (removed && self->retainAfterRemove) {
                events::table<events::UnknownVTable>(removed)->AddRef(removed);
                self->delayedReference = removed;
            }
        }
        if (removed && self->invokeDuringRemove) {
            events::table<HandlerVTable>(removed)->Invoke(removed, self, nullptr);
        }
        events::release(removed);
        return S_OK;
    }
    inline static events::NotifyVTable kVTable{
        {{&query, &add, &release}, &getIids, &getName, &getTrust}, &subscribe, &unsubscribe};
};

struct CaptureContext {
    AppleMusicPositionTracker tracker;
    std::atomic<int> callbacks{0};
    std::atomic<bool> entered{false};
    std::atomic<bool> allowReturn{true};
    std::int64_t capturedQpc = 0;
};

void capture(void* raw, void* sender, void*, std::int64_t qpc) noexcept {
    auto* context = static_cast<CaptureContext*>(raw);
    context->entered.store(true);
    while (!context->allowReturn.load()) Sleep(1);
    const auto* source = static_cast<FakeObservable*>(sender);
    context->capturedQpc = qpc;
    context->tracker.event(1, source->position100ns, qpc, true);
    ++context->callbacks;
}

void testFakeNotifyAndPositionGetter() {
    FakeObservable source;
    CaptureContext context;
    context.tracker.reset(1);
    events::PropertyChangedSubscription subscription;
    require(subscription.attach(&source, &context, &capture), "a matching INotifyPropertyChanged interface must attach");
    source.position100ns = 420000000;
    source.fire();
    auto sample = context.tracker.query(1, 420000000, events::qpcNow(), true);
    require(context.callbacks == 1 && sample.fromEvent && sample.position100ns == 420000000 &&
            sample.queryQpc == context.capturedQpc,
            "the fake position getter must be paired with the event entry QPC");
    const auto eventQpc = sample.queryQpc;
    sample = context.tracker.query(1, 420000000, events::qpcNow(), true);
    require(sample.queryQpc == eventQpc, "repeat queries through the real delegate path must preserve the event time");
    require(subscription.close(100), "normal removal must release the delegate and notify interface");
    require(source.references == 1 && source.removals == 1, "close must leave no retained publisher references");
    source.fire();
    require(context.callbacks == 1, "removed callbacks must not run");
}

void testUnsupportedAndLegacyPublishers() {
    FakeObservable source;
    source.supportsNotify = false;
    CaptureContext context;
    events::PropertyChangedSubscription unsupported;
    require(!unsupported.attach(&source, &context, &capture), "unsupported interfaces must fall back without subscribing");
    require(unsupported.close(0) && source.references == 1, "unsupported publishers must not leak references");
    source.supportsNotify = true;
    source.legacyOnly = true;
    events::PropertyChangedSubscription legacy;
    require(legacy.attach(&source, &context, &capture), "legacy Windows XAML publishers should remain compatible");
    require(legacy.close(100), "legacy callbacks must use the same safe cleanup");
    source.legacyOnly = false;
    source.failAdd = true;
    events::PropertyChangedSubscription failed;
    require(!failed.attach(&source, &context, &capture) && failed.close(0), "failed registration must fail safely");
    require(source.references == 1, "failed registration must release the queried notify interface");
}

void testFailedRemovalDisablesAndRetainsCallback() {
    FakeObservable source;
    CaptureContext context;
    events::PropertyChangedSubscription subscription;
    require(subscription.attach(&source, &context, &capture), "fake subscription should attach");
    source.failRemove = true;
    require(!subscription.close(0), "failed event removal must prevent DLL unloading");
    source.fire();
    require(context.callbacks == 0, "a retained subscription must be disabled immediately");
    source.failRemove = false;
    require(subscription.close(100), "cleanup should recover when removal becomes possible");
}

void testExternallyRetainedDelegateBlocksUnload() {
    FakeObservable source;
    source.retainAfterRemove = true;
    CaptureContext context;
    events::PropertyChangedSubscription subscription;
    require(subscription.attach(&source, &context, &capture), "fake subscription should attach");
    require(!subscription.close(0), "even an idle external delegate reference must prevent DLL unloading");
    events::table<HandlerVTable>(source.delayedReference)->Invoke(source.delayedReference, &source, nullptr);
    require(context.callbacks == 0, "delayed retained delegates must no longer access their owner");
    source.releaseDelayedReference();
    require(subscription.close(100), "the disabled delegate may be freed after its final external release");
}

void testInFlightCallbackMustDrain() {
    FakeObservable source;
    CaptureContext context;
    context.allowReturn.store(false);
    events::PropertyChangedSubscription subscription;
    require(subscription.attach(&source, &context, &capture), "fake subscription should attach");
    std::thread publisher([&] { source.fire(); });
    while (!context.entered.load()) Sleep(1);
    const bool prematurelyClosed = subscription.close(5);
    context.allowReturn.store(true);
    publisher.join();
    require(!prematurelyClosed, "an in-flight callback must prevent DLL unloading until it returns");
    require(subscription.close(100), "cleanup may complete once the in-flight callback releases its delegate");
}

void testSynchronousRegistrationAndRemovalCallbacks() {
    FakeObservable source;
    source.position100ns = 420000000;
    source.fireDuringAdd = true;
    source.invokeDuringRemove = true;
    CaptureContext context;
    context.tracker.reset(1);
    events::PropertyChangedSubscription subscription;
    require(subscription.attach(&source, &context, &capture), "synchronous registration callbacks must have a fully initialized context");
    require(context.callbacks == 1, "a callback raised during registration must capture its position");
    require(subscription.close(100), "reentrant removal notifications must not deadlock cleanup");
    require(context.callbacks == 1, "callbacks raised synchronously by remove must be disabled before remove is called");
    require(source.references == 1, "reentrant cleanup must still release all publisher references");
}

}

int main() {
    try {
        testEventTimeIsReused();
        testFallbackDoesNotPretendToUpdate();
        testPauseAndResumeNeverReusePauseAge();
        testTrackChangesRejectOldCallbacks();
        testMissedEventFallsBackSafely();
        testDelayedSnapshotCannotRestorePreviousIdentity();
        testDuplicateNotificationsDoNotRestampPosition();
        testQueryBeforeNotificationPreservesRealEventTime();
        testFakeNotifyAndPositionGetter();
        testUnsupportedAndLegacyPublishers();
        testFailedRemovalDisablesAndRetainsCallback();
        testExternallyRetainedDelegateBlocksUnload();
        testInFlightCallbackMustDrain();
        testSynchronousRegistrationAndRemovalCallbacks();
        std::cout << "All Apple Music position tracker tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Position tracker test failed: " << error.what() << '\n';
        return 1;
    }
}
