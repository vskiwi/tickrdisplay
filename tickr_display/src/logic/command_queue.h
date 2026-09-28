#pragma once
// Thread-safe command queue between network callbacks (async_tcp task, MQTT
// callback) and the main loop. Network handlers must never block (e-ink refresh
// takes 2-3 s, melodies up to 15 s, delay() ...), so they only enqueue a small
// Command and answer immediately; loop() pops and executes.
//
// Ownership: command_queue_push() copies the string payload into a heap buffer;
// command_execute() (or command_free()) releases it.

#include <stdint.h>
#include <stddef.h>

#define COMMAND_QUEUE_DEPTH   8

enum CommandType : uint8_t {
    CMD_NONE = 0,
    CMD_PAYLOAD,        // str = JSON screen payload (already validated by the producer)
    CMD_LED_RGB,        // a,b,c = r,g,b
    CMD_LED_SET_R,      // a = value
    CMD_LED_SET_G,
    CMD_LED_SET_B,
    CMD_LED_BLINK,      // a,b,c = r,g,b (0/1); on for 500 ms then off
    CMD_VOLUME,         // a = volume
    CMD_PLAY_RTTTL,     // str = melody
    CMD_BEEP,           // a = preset: PayloadSound
    CMD_SCREEN_STATUS,  // str = status text
    CMD_SCREEN_MESSAGE, // str = "title\nmessage"
};

enum CommandSource : uint8_t {
    SRC_HTTP = 0,
    SRC_MQTT,
    SRC_INTERNAL,
};

struct Command {
    CommandType   type;
    CommandSource src;
    uint8_t a, b, c;
    uint16_t len;       // strlen(str)
    char* str;          // heap copy (may be NULL)
};

// Must be called once before any other function (from setup()).
void command_queue_init();

// Enqueue a command. `str` (may be NULL) is copied; `len` bytes, at most `max_len`.
// Returns false if the queue is full, the string exceeds `max_len`, or allocation failed
// (nothing is enqueued in that case; the caller keeps ownership of its data).
bool command_queue_push(CommandType type, CommandSource src,
                        const char* str, size_t len, size_t max_len,
                        uint8_t a = 0, uint8_t b = 0, uint8_t c = 0);

// Convenience for the common case (no string).
inline bool command_queue_push_simple(CommandType type, CommandSource src,
                                      uint8_t a = 0, uint8_t b = 0, uint8_t c = 0) {
    return command_queue_push(type, src, nullptr, 0, 0, a, b, c);
}

// Non-blocking pop. Returns false if empty.
bool command_queue_pop(Command* out);

// Release the heap payload of a popped command.
void command_free(Command* cmd);

// Current number of queued commands (for diagnostics).
unsigned command_queue_depth();

// Total commands dropped because the queue was full (diagnostics).
uint32_t command_queue_dropped();

// Execute one command on the calling (main loop) task. Frees the payload.
void command_execute(Command* cmd);

// Human-readable source name.
const char* command_source_str(CommandSource src);
