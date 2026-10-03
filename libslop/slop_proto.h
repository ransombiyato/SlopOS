/*
 * slop_proto.h -- the SlopOS compositor wire protocol.
 *
 * SlopOS native apps draw into their own shared-memory surface and hand it
 * to the shell, which is the compositor. This is deliberately not X11 or
 * Wayland: it is a tiny, versioned, fixed-message protocol over one AF_UNIX
 * socket, with framebuffers passed as memfds via SCM_RIGHTS.
 *
 * The shell is the only process that owns the real screen. Each app is a
 * client with a window; the shell composites, routes input with window-local
 * coordinates, and tells apps when they are focused, resized or closed.
 *
 * Everything degrades gracefully: if an app cannot reach the compositor it
 * falls back to owning the framebuffer directly (the pre-0.2 behaviour). That
 * keeps single-process debugging and the boot splash working.
 */
#ifndef SLOP_PROTO_H
#define SLOP_PROTO_H

#include <stdint.h>

#define SLOP_COMPOSITOR_SOCK "/run/slop/compositor.sock"
#define SLOP_PROTO_MAGIC     0x534c4f50u   /* 'SLOP' */
#define SLOP_PROTO_VERSION   1u

typedef enum {
    SLOP_MSG_NONE = 0,
    SLOP_MSG_HELLO,      /* client -> server: I am a window */
    SLOP_MSG_WELCOME,    /* server -> client: your window id */
    SLOP_MSG_SURFACE,    /* client -> server: here is (a new) framebuffer fd */
    SLOP_MSG_FRAME,      /* client -> server: the surface has new pixels */
    SLOP_MSG_CONFIGURE,  /* server -> client: please resize to w x h */
    SLOP_MSG_EVENT,      /* server -> client: input, window-local coords */
    SLOP_MSG_FOCUS,      /* server -> client: focus changed */
    SLOP_MSG_CLOSE,      /* server -> client: close yourself */
    SLOP_MSG_BYE,        /* client -> server: I am leaving */
    SLOP_MSG_SET_TITLE,  /* client -> server: change my title */
    SLOP_MSG_SET_ICON,   /* client -> server: change my icon id */
    SLOP_MSG_PING,       /* either way: liveness */
} slop_msg_type;

/* Input event kinds carried in slop_msg.kind when type == SLOP_MSG_EVENT. */
enum {
    SLOP_PEV_MOTION = 1,
    SLOP_PEV_BUTTON_DOWN,
    SLOP_PEV_BUTTON_UP,
    SLOP_PEV_WHEEL,
    SLOP_PEV_KEY,
    SLOP_PEV_TEXT,
    SLOP_PEV_LEAVE,
};

typedef struct {
    uint32_t type;
    uint32_t magic;
    int32_t  version;
    int32_t  id;          /* window id (server assigns in WELCOME) */
    int32_t  w, h;        /* surface size */
    int32_t  kind;        /* SLOP_PEV_* for EVENT */
    int32_t  x, y;        /* window-local pointer for EVENT */
    int32_t  button;
    int32_t  wheel;
    int32_t  code;        /* raw evdev keycode */
    int32_t  key;         /* logical slop key */
    int32_t  mods;        /* SLOP_MOD_* */
    int32_t  ch;          /* translated character */
    int32_t  focused;
    int32_t  icon;        /* dock icon id for SET_ICON */
    char     title[96];
} slop_msg;

#endif /* SLOP_PROTO_H */
