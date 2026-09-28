#pragma once
// Fleet layout document (docs/MULTI_DEVICE.md "The shelf layout") - validation only.
//
// The browser writes the same JSON to every USB member (PUT /api/layout) and
// reads it back from all of them, keeping the newest `updated_at`. Each device
// still holds its *own* slot in config.json (POST /api/identity); this file is
// the second store that also remembers slots and names of devices that are
// asleep or off. Schema as built (unknown keys are ignored, never rejected):
//
//   {"v":1,"updated_at":<ms since epoch, browser clock>,"by":"tickr-XXXXXX",
//    "devices":{"tickr-XXXXXX":{"x":0,"y":0,"name":"Shelf-Left"},...}}
//
// Rules: object; v == 1; updated_at a non-negative number; devices an object
// with at most LAYOUT_MAX_DEVICES entries; every entry an object with integer
// x/y in 0..LAYOUT_SLOT_MAX and an optional string name <= LAYOUT_NAME_MAX.
// The optional top-level key "groups" (no schema bump):
//   "groups":{"Kitchen":["tickr-A1B2C3","tickr-D4E5F6"],...}
// at most LAYOUT_MAX_GROUPS names of 1..LAYOUT_NAME_MAX printable ASCII
// chars, each an array of <= LAYOUT_MAX_DEVICES ids. The panel resolves a
// group to ids itself (docs/MULTI_DEVICE.md "One page, every device") - the device only stores it.
#include <stddef.h>
#include <stdint.h>

#define LAYOUT_MAX_LEN      4096   // bytes of JSON accepted by PUT /api/layout
#define LAYOUT_MAX_DEVICES  64
#define LAYOUT_MAX_GROUPS   8
#define LAYOUT_SLOT_MAX     15     // = CONFIG_LAYOUT_MAX
#define LAYOUT_NAME_MAX     31     // = CONFIG_NAME_LEN - 1

// Parses and checks `json` (len bytes, need not be NUL-terminated). On
// success returns true and stores the device count in *devices (may be NULL);
// on failure returns false with a short reason in err.
bool layout_validate(const char* json, size_t len, size_t* devices, char* err, size_t err_len);
