// Copyright 2021 Manna Harbour
// https://github.com/manna-harbour/miryoku

// Prospector display brightness action codes.
// Used in the keymap as &pbl PBL_INC etc. (dongle+prospector builds only).
// Non-prospector builds see U_NA via the fallback macros below.
// Bound on MEDIA (left-hand inner-index column for INC/DEC, right-hand
// inner-index for TOG) and on FUN, immediately left of the right-hand
// home-row shift (INC on the home row, DEC directly below it).
#define PBL_TOG 0
#define PBL_INC 1
#define PBL_DEC 2

#ifdef CONFIG_SHIELD_PROSPECTOR_ADAPTER
#define U_PBL_TOG &pbl PBL_TOG
#define U_PBL_INC &pbl PBL_INC
#define U_PBL_DEC &pbl PBL_DEC
#else
// Halves and dongle-without-display builds: no behavior node, fall back to no-op.
#define U_PBL_TOG U_NA
#define U_PBL_INC U_NA
#define U_PBL_DEC U_NA
#endif

#define MIRYOKU_LAYER_FUN \
&kp F12,           &kp F7,            &kp F8,            &kp F9,            &kp PSCRN,         U_NA,              &u_to_U_BASE,      &u_to_U_EXTRA,     &u_to_U_TAP,       U_BOOT,            \
&kp F11,           &kp F4,            &kp F5,            &kp F6,            &kp SLCK,          U_PBL_INC,         &kp LSHFT,         &kp LCTRL,         &kp LALT,          &kp LGUI,          \
&kp F10,           &kp F1,            &kp F2,            &kp F3,            &kp PAUSE_BREAK,   U_PBL_DEC,         &u_to_U_FUN,       &u_to_U_MEDIA,     &kp RALT,          U_NA,              \
U_NP,              U_NP,              &kp K_APP,         &kp SPACE,         &kp TAB,           U_NA,              U_NA,              U_NA,              U_NP,              U_NP

#define MIRYOKU_LAYER_MEDIA \
U_BOOT,            &u_to_U_TAP,       &u_to_U_EXTRA,     &u_to_U_BASE,      &studio_unlock,    U_PBL_TOG,         U_RGB_EFF,         U_RGB_HUI,         U_RGB_SAI,         U_RGB_BRI,         \
&kp LGUI,          &kp LALT,          &kp LCTRL,         &kp LSHFT,         U_PBL_INC,         U_EP_TOG,          &kp C_PREV,        &kp C_VOL_DN,      &kp C_VOL_UP,      &kp C_NEXT,        \
U_NA,              &kp RALT,          &u_to_U_FUN,       &u_to_U_MEDIA,     U_PBL_DEC,         &u_out_tog,        &u_bt_sel_0,       &u_bt_sel_1,       &u_bt_sel_2,       &u_bt_sel_3,       \
U_NP,              U_NP,              U_NA,              U_NA,              U_NA,              &kp C_STOP,        &kp C_PP,          &kp C_MUTE,        U_NP,              U_NP
