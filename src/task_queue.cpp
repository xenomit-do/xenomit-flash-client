// Producer/consumer task engine modelled on InProcessPlayer (§4-§5): owned jobs
// are queued for the Flash STA, callers wait on a promise-like completion with
// a bound or keep an asynchronous ticket. Frame calls are completed by the
// bridge's xenomitResult push instead of polling.
#include "host.h"

#include <deque>
#include <map>

namespace xf {
namespace {

struct Pending {
    UiJob job;
    CompletionPtr done;
    ULONGLONG deadline = 0;
};

constexpr size_t QueueLimit = 256;
constexpr ULONGLONG TicketLifetime = 10000;

std::mutex g_queue_mutex;
std::deque<Pending> g_queue;
HWND g_target = nullptr;
UINT g_message = 0;
std::atomic<DWORD> g_ui_thread{0};

std::mutex g_frames_mutex;
std::map<uint64_t, CompletionPtr> g_frames;
std::atomic<uint64_t> g_frame_id{0};

std::mutex g_tickets_mutex;
std::map<uint64_t, CompletionPtr> g_tickets;
std::atomic<uint64_t> g_ticket_id{0};

CompletionPtr finished(XfCode code, std::string text) {
    auto done = std::make_shared<Completion>();
    done->complete({code, std::move(text)});
    return done;
}

// Frame calls the bridge never answered (reload, removed clip) must not leak
// or keep an async caller pending forever.
void sweep_frames() {
    auto now = GetTickCount64();
    std::vector<CompletionPtr> expired;
    {
        std::lock_guard<std::mutex> lock(g_frames_mutex);
        for (auto it = g_frames.begin(); it != g_frames.end();) {
            if (now - it->second->created > TicketLifetime) {
                expired.push_back(it->second);
                it = g_frames.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto &done : expired)
        done->complete({XF_TIMEOUT_UNKNOWN, "frame call result was never reported; execution unknown"});
}

} // namespace

void Completion::complete(Outcome outcome) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (done_) return;
        done_ = true;
        outcome_ = std::move(outcome);
    }
    ready_.notify_all();
}

bool Completion::wait(DWORD timeout_ms, Outcome &out) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!ready_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return done_; })) {
        cancelled_ = true;
        return false;
    }
    out = outcome_;
    return true;
}

bool Completion::done() {
    std::lock_guard<std::mutex> lock(mutex_);
    return done_;
}

void queue_attach(HWND window, UINT message) {
    std::lock_guard<std::mutex> lock(g_queue_mutex);
    g_target = window;
    g_message = message;
}

void queue_detach(const char *reason) {
    std::deque<Pending> abandoned;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        g_target = nullptr;
        abandoned.swap(g_queue);
    }
    for (auto &item : abandoned) item.done->complete({XF_SHUTTING_DOWN, reason});
    std::map<uint64_t, CompletionPtr> frames;
    {
        std::lock_guard<std::mutex> lock(g_frames_mutex);
        frames.swap(g_frames);
    }
    for (auto &entry : frames) entry.second->complete({XF_SHUTTING_DOWN, reason});
}

bool on_ui_thread() { return g_ui_thread.load() == GetCurrentThreadId(); }
void set_ui_thread(DWORD id) { g_ui_thread = id; }

void queue_drain() {
    std::deque<Pending> work;
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        work.swap(g_queue);
    }
    for (auto &item : work) {
        if (item.done->cancelled() || GetTickCount64() > item.deadline) {
            item.done->complete({XF_NOT_READY, "call expired before execution"});
            continue;
        }
        try {
            item.job(item.done);
        } catch (const std::exception &error) {
            item.done->complete({XF_FLASH_ERROR, std::string("host error: ") + error.what()});
        } catch (...) {
            item.done->complete({XF_FLASH_ERROR, "host error"});
        }
    }
}

CompletionPtr begin_ui(UiJob job, DWORD deadline_ms, CompletionPtr done) {
    if (!done) done = std::make_shared<Completion>();
    auto reject = [&](XfCode code, const char *text) {
        done->complete({code, text});
        return done;
    };
    if (on_ui_thread()) {
        // Re-entrant call from the STA (for example a log callback): run inline.
        job(done);
        return done;
    }
    {
        std::lock_guard<std::mutex> lock(g_queue_mutex);
        if (!g_target) return reject(XF_NOT_READY, "player is not running");
        if (g_queue.size() >= QueueLimit) return reject(XF_QUEUE_FULL, "player call queue full");
        g_queue.push_back({std::move(job), done, GetTickCount64() + deadline_ms});
        if (!PostMessageW(g_target, g_message, 0, 0)) {
            g_queue.pop_back();
            return reject(XF_NOT_READY, "player window unavailable");
        }
    }
    return done;
}

CompletionPtr begin_ui_call(std::function<Outcome()> fn, DWORD deadline_ms) {
    return begin_ui([fn = std::move(fn)](const CompletionPtr &done) { done->complete(fn()); }, deadline_ms);
}

CompletionPtr begin_frame_call(const std::string &target, const std::string &method,
                               const std::string &json_args, DWORD deadline_ms) {
    if (on_ui_thread()) return finished(XF_INVALID, "a frame call cannot wait on the Flash thread");
    sweep_frames();
    auto id = ++g_frame_id;
    auto done = std::make_shared<Completion>();
    {
        std::lock_guard<std::mutex> lock(g_frames_mutex);
        g_frames[id] = done;
    }
    // The queue item shares the frame completion: an expired or rejected
    // submission finishes the call, a queued one waits for xenomitResult.
    return begin_ui([id, target, method, json_args](const CompletionPtr &call) {
        std::string parsed, error;
        bool ok = flash_call("queueAS", {std::to_string(id), target, method, json_args}, parsed, error);
        if (ok && parsed == "queued") return;
        {
            std::lock_guard<std::mutex> lock(g_frames_mutex);
            g_frames.erase(id);
        }
        call->complete({XF_FLASH_ERROR, ok ? parsed : error});
    }, deadline_ms, done);
}

void complete_frame_call(uint64_t id, std::string value) {
    CompletionPtr done;
    {
        std::lock_guard<std::mutex> lock(g_frames_mutex);
        auto it = g_frames.find(id);
        if (it == g_frames.end()) return;
        done = it->second;
        g_frames.erase(it);
    }
    done->complete({XF_OK, std::move(value)});
}

XfCode finish(const CompletionPtr &done, uint32_t flags, DWORD timeout_ms, Outcome &out, uint64_t *ticket) {
    if (flags & XF_ASYNC) {
        if (!ticket) return XF_INVALID;
        if (done->done()) {
            // Immediate rejection (queue full, not running) is reported now.
            Outcome early;
            done->wait(0, early);
            if (early.code != XF_OK) {
                out = early;
                return early.code;
            }
        }
        auto id = ++g_ticket_id;
        std::lock_guard<std::mutex> lock(g_tickets_mutex);
        auto now = GetTickCount64();
        for (auto it = g_tickets.begin(); it != g_tickets.end();) {
            if (now - it->second->created > TicketLifetime && it->second->done()) it = g_tickets.erase(it);
            else ++it;
        }
        g_tickets[id] = done;
        *ticket = id;
        return XF_OK;
    }
    if (!done->wait(timeout_ms, out)) {
        out = {XF_TIMEOUT_UNKNOWN, "call timed out; execution may have started; do not retry automatically"};
    }
    return out.code;
}

XfCode take_ticket(uint64_t ticket, Outcome &out) {
    CompletionPtr done;
    {
        std::lock_guard<std::mutex> lock(g_tickets_mutex);
        auto it = g_tickets.find(ticket);
        if (it == g_tickets.end()) return XF_UNKNOWN_TICKET;
        done = it->second;
        if (!done->done()) {
            sweep_frames();
            if (!done->done()) return XF_PENDING;
        }
        g_tickets.erase(it);
    }
    done->wait(0, out);
    return out.code;
}

Outcome run_ui(std::function<Outcome()> fn, DWORD timeout_ms) {
    Outcome out;
    finish(begin_ui_call(std::move(fn), timeout_ms), 0, timeout_ms, out, nullptr);
    return out;
}

} // namespace xf
