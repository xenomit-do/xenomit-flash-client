/*
 * xenomit_flash.dll: in-process Flash ActiveX host (C ABI).
 *
 * The host is a process-wide singleton, like InProcessPlayer's JNI host. A caller
 * loads the DLL into its own process, starts the host once and then calls the
 * exported functions directly from any thread. All Flash/COM work runs on the
 * host's own STA thread; exported calls post owned tasks to it and wait with a
 * bound. Exceptions never cross this boundary.
 *
 * Strings are UTF-8 and NUL-terminated. Text results are returned in an XfBuf
 * allocated by the DLL; release them with xf_free. A NULL XfBuf* discards the
 * result. Calls that return XF_TIMEOUT_UNKNOWN may have executed: never retry
 * them automatically.
 */
#ifndef XENOMIT_FLASH_H
#define XENOMIT_FLASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(XENOMIT_FLASH_BUILD)
#define XF_API __declspec(dllexport)
#else
#define XF_API __declspec(dllimport)
#endif
#define XF_CALL __cdecl

/* Bumped on any incompatible change to a struct or function below. */
#define XF_API_VERSION 1

typedef enum XfCode {
    XF_OK = 0,
    /* The request was submitted but its outcome is unknown. Do not retry. */
    XF_TIMEOUT_UNKNOWN = 1,
    XF_QUEUE_FULL = 2,
    /* Host not started, Flash not loaded, or the game is not ready. */
    XF_NOT_READY = 3,
    /* ActionScript or Flash reported an error; the XfBuf holds its text. */
    XF_FLASH_ERROR = 4,
    XF_INVALID = 5,
    XF_ALREADY_RUNNING = 6,
    XF_SHUTTING_DOWN = 7,
    /* xf_take_result: the asynchronous request has not finished yet. */
    XF_PENDING = 8,
    XF_START_FAILED = 9,
    XF_UNKNOWN_TICKET = 10
} XfCode;

typedef struct XfBuf {
    char *data;
    size_t len;
} XfBuf;

/* level: 0 info, 1 warning, 2 error. Called from host threads; must not block
 * and must not call back into the host. */
typedef void(XF_CALL *XfLogFn)(void *context, int32_t level, const char *utf8);

typedef struct XfConfig {
    uint32_t size; /* sizeof(XfConfig) */
    /* Bootstrap. Session values never leave this process. */
    const char *movie;      /* preloader URL or local SWF path (required) */
    const char *base_url;   /* page base URL, may be NULL */
    const char *sid;        /* session cookie, may be NULL */
    const char *flash_vars; /* k=v&k=v, may be NULL */
    int32_t width, height;  /* stage size, <=0 uses 1280x800 */
    /* Options. NULL/0 selects the default. */
    const char *backend;          /* "cached" (default) or "native" */
    const char *native_discovery; /* auto|anchor|pattern|parity */
    const char *quality;          /* low (default)|medium|high|best */
    const char *wmode;            /* window (default)|direct|opaque */
    const char *ocx_path;         /* default: Flash.ocx beside the DLL */
    const char *user_agent;       /* default: BigpointClient/1.6.9 */
    const char *title;            /* window title */
    int32_t hidden;               /* start hidden */
    int32_t hidden_draw_bootstrap;/* offscreen OleDraw until game readiness */
    int32_t max_fps;              /* 0: game default (60), else 1..100 */
    int32_t local_proxy_port;     /* 0: none, else 127.0.0.1:<port> */
    const char *clear_cache;      /* purge URL-cache entries containing this */
    const char *game_ports;       /* comma list of game socket ports, default "5001" */
    int32_t publish_named_frame;  /* LiveFrame as Local\xenomit_state_<pid> */
    int32_t pipe_server;          /* serve \\.\pipe\xenomit_flash_<pid> */
    XfLogFn log;
    void *log_context;
} XfConfig;

typedef struct XfStatus {
    uint32_t size; /* set by the caller to sizeof(XfStatus) */
    uint8_t player, bridge, ready, native;
    uint64_t frames, fps_milli;
    uint64_t captures, capture_us_avg, capture_us_max, state_age_ms;
    uint64_t item_updates, item_refreshes;
    uint64_t bootstrap_draws, bootstrap_failures;
    uint64_t parity_checks, parity_mismatches;
    char native_root[16];
    /* Network observation (InProcessPlayer lastInternetReadTime/lastClientReadTime,
     * with ages and byte counts). Ages are UINT64_MAX when never observed. */
    uint64_t net_reads, net_read_age_ms;
    uint64_t game_socket, game_recv, game_recv_bytes, game_recv_age_ms;
    uint64_t vfs_hits, blocked_requests;
    double cpu_percent;
    uint64_t working_set_bytes;
} XfStatus;

/* Flags for calls that execute in the game. */
#define XF_FRAME 1u /* run in the next ENTER_FRAME (bounded FIFO) */
#define XF_ASYNC 2u /* return a ticket at once; collect with xf_take_result */

/* Mouse kinds for xf_mouse. */
#define XF_MOUSE_MOVE 0
#define XF_MOUSE_DOWN 1
#define XF_MOUSE_UP 2
#define XF_MOUSE_CLICK 3
/* Key kinds for xf_key. */
#define XF_KEY_DOWN 0
#define XF_KEY_UP 1
#define XF_KEY_CLICK 2

XF_API uint32_t XF_CALL xf_api_version(void);
XF_API void XF_CALL xf_free(XfBuf *buffer);

/* Lifecycle. xf_start returns once Flash is loaded and playing, or failed. */
XF_API XfCode XF_CALL xf_start(const XfConfig *config, XfBuf *error);
XF_API XfCode XF_CALL xf_stop(uint32_t timeout_ms);
/* XF_OK once the host thread has ended (or never started), XF_PENDING on
 * timeout. exit_code: 0, or the start failure code 2..7 of the former exe. */
XF_API XfCode XF_CALL xf_wait(uint32_t timeout_ms, int32_t *exit_code);
XF_API XfCode XF_CALL xf_reload(XfBuf *error);

/* State. Neither call touches the Flash thread. */
XF_API XfCode XF_CALL xf_status(XfStatus *status);
/* Version 2 LiveFrame (seqlock) owned by the host; valid until process exit. */
XF_API XfCode XF_CALL xf_live_frame(const uint8_t **base, size_t *len);
/* Cached-capture or reflected snapshot JSON (STATE). */
XF_API XfCode XF_CALL xf_state(const char *target, XfBuf *result);
XF_API XfCode XF_CALL xf_inspect(const char *target, XfBuf *result);

/* Calls. XF_OK means the call ran: result holds the ActionScript JSON envelope
 * {"status":"ok"|"error",...}. Any other code puts the error text in result.
 * With XF_ASYNC, *ticket receives the request ticket and result is unused
 * unless submission failed. timeout_ms 0 selects the default (4500 ms in a
 * frame, 5000 ms otherwise). */
XF_API XfCode XF_CALL xf_invoke(const char *target, const char *method, const char *json_args,
                                uint32_t flags, uint32_t timeout_ms, XfBuf *result, uint64_t *ticket);
XF_API XfCode XF_CALL xf_game_command(const char *command_json, uint32_t flags, uint32_t timeout_ms,
                                      XfBuf *result, uint64_t *ticket);
XF_API XfCode XF_CALL xf_take_result(uint64_t ticket, XfBuf *result);

/* Direct actions (InProcessPlayer moveShip/selectEntity/collectBox/useItem/refine/
 * sendNotification). Always frame-bound; XF_ASYNC is honoured. */
XF_API XfCode XF_CALL xf_move(int32_t x, int32_t y, uint32_t flags, XfBuf *result, uint64_t *ticket);
XF_API XfCode XF_CALL xf_stop_hero(uint32_t flags, XfBuf *result, uint64_t *ticket);
XF_API XfCode XF_CALL xf_select(int32_t entity_id, int32_t attack, uint32_t flags, XfBuf *result, uint64_t *ticket);
XF_API XfCode XF_CALL xf_deselect(uint32_t flags, XfBuf *result, uint64_t *ticket);
/* Moves to (x, y) and collects the entity there; XF_AT_ENTITY for both
 * coordinates uses the entity's own position. */
#define XF_AT_ENTITY INT32_MIN
XF_API XfCode XF_CALL xf_collect(int32_t entity_id, int32_t x, int32_t y, uint32_t flags, XfBuf *result, uint64_t *ticket);
XF_API XfCode XF_CALL xf_use_item(const char *item_id, int32_t activate, uint32_t flags, XfBuf *result, uint64_t *ticket);
XF_API XfCode XF_CALL xf_refine(int32_t ore_type, uint64_t amount, uint32_t flags, XfBuf *result, uint64_t *ticket);
XF_API XfCode XF_CALL xf_notify(const char *name, const char *json_args, uint32_t flags, XfBuf *result, uint64_t *ticket);
/* {"move":true,...}: which direct actions resolved against the running game. */
XF_API XfCode XF_CALL xf_caps(XfBuf *result);

/* Input, delivered to the Flash control window on the STA. */
XF_API XfCode XF_CALL xf_mouse(int32_t kind, int32_t x, int32_t y);
XF_API XfCode XF_CALL xf_key(uint32_t virtual_key, int32_t kind);
XF_API XfCode XF_CALL xf_text(const char *utf8);
XF_API XfCode XF_CALL xf_paste(const char *utf8);
/* 0 drops real keyboard/mouse input to the game window; injected input stays. */
XF_API XfCode XF_CALL xf_set_user_input(int32_t enabled);

/* Window and rendering. */
XF_API XfCode XF_CALL xf_set_visible(int32_t visible);
XF_API XfCode XF_CALL xf_set_position(int32_t x, int32_t y);
XF_API XfCode XF_CALL xf_set_size(int32_t width, int32_t height);
XF_API XfCode XF_CALL xf_set_client_size(int32_t width, int32_t height);
XF_API XfCode XF_CALL xf_set_min_client_size(int32_t width, int32_t height);
XF_API XfCode XF_CALL xf_set_minimized(int32_t minimized);
XF_API XfCode XF_CALL xf_set_transparency(int32_t alpha); /* 0..255, 255 opaque */
XF_API XfCode XF_CALL xf_set_max_fps(int32_t fps);        /* 0 default, 1..100 */
XF_API XfCode XF_CALL xf_set_volume(int32_t percent);     /* 0..100 */
XF_API XfCode XF_CALL xf_set_quality(const char *quality);
XF_API XfCode XF_CALL xf_set_backend(const char *backend, XfBuf *error);

/* Network and maintenance. */
XF_API XfCode XF_CALL xf_set_blocking_patterns(const char *const *patterns, size_t count);
XF_API XfCode XF_CALL xf_clear_cache(const char *substring, uint32_t *deleted);
XF_API XfCode XF_CALL xf_trim(void);

/* Guarded memory reads (access violations are caught). Not synchronized
 * with the Flash thread: VM objects can change while they are read. Hot
 * state belongs to the LiveFrame. */
XF_API XfCode XF_CALL xf_read(uint64_t address, void *out, size_t len);
/* First matches of pattern (mask byte 0xff = compare) in committed private
 * read/write regions up to 32 MiB, like InProcessPlayer queryBytes. */
XF_API XfCode XF_CALL xf_query_bytes(const uint8_t *pattern, const uint8_t *mask, size_t len,
                                     uint64_t *out, size_t max, size_t *count);

/* The legacy pipe command grammar, executed in-process ("OK ..."/"ERR ..."). */
XF_API XfCode XF_CALL xf_exchange(const char *command, XfBuf *reply);

#ifdef __cplusplus
}
#endif

#endif
