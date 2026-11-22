// fake_alsa.c - a stand-in for libasound.so.2 in the plugin tests (0.7.0). The plugin loads ALSA
// with dlopen("libasound.so.2"); plugin_test loads this library first (built with that soname),
// so the plugin's client "opens" on a computer without ALSA, and every event it sends is written
// to $FAKE_ALSA_TRACE, one line each, stamped with the test's audio time:
//
//   <seconds> <port> on|off <channel 1-16> <note> <velocity>
//   <seconds> <port> cc <channel 1-16> <controller> <value>
//
// Ports are named as the plugin names them, spaces as underscores (MIDI_Out, Linn_Out). The test
// sets the time with fake_alsa_time() before each block. Nothing is ever received.
//
// SPDX-License-Identifier: MIT
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uint8_t client, port; } fa_addr;
typedef struct
{
    uint8_t type, flags, tag, queue;
    uint32_t time[2];
    fa_addr source, dest;
    union
    {
        struct { uint8_t channel, note, velocity, off_velocity; uint32_t duration; } note;
        struct { uint8_t channel, unused[3]; uint32_t param; int32_t value; } control;
        uint8_t raw[12];
    } data;
} fa_event;

typedef struct { char name[64]; char ports[8][32]; int nports; } fa_client;

static double fa_now = 0;
static FILE* fa_trace = NULL;

void fake_alsa_time(double seconds) { fa_now = seconds; }

static FILE* trace_file(void)
{
    if (!fa_trace)
    {
        const char* path = getenv("FAKE_ALSA_TRACE");
        if (path && (fa_trace = fopen(path, "w")) != NULL) setvbuf(fa_trace, NULL, _IOLBF, 0);
    }
    return fa_trace;
}

int snd_seq_open(void** handle, const char* name, int streams, int mode)
{
    (void)name; (void)streams; (void)mode;
    *handle = calloc(1, sizeof(fa_client));
    return *handle ? 0 : -12;
}
int snd_seq_set_client_name(void* h, const char* name)
{
    strncpy(((fa_client*)h)->name, name, sizeof(((fa_client*)h)->name) - 1);
    return 0;
}
int snd_seq_create_simple_port(void* h, const char* name, unsigned caps, unsigned type)
{
    fa_client* c = (fa_client*)h;
    (void)caps; (void)type;
    if (c->nports >= 8) return -1;
    strncpy(c->ports[c->nports], name, sizeof(c->ports[0]) - 1);
    for (char* s = c->ports[c->nports]; *s; ++s) if (*s == ' ') *s = '_';
    return c->nports++;
}
int snd_seq_event_output_direct(void* h, fa_event* ev)
{
    fa_client* c = (fa_client*)h;
    FILE* f = trace_file();
    const char* port = ev->source.port < c->nports ? c->ports[ev->source.port] : "?";
    if (f)
    {
        if (ev->type == 6 || ev->type == 7)
            fprintf(f, "%.4f %s %s %d %d %d\n", fa_now, port, ev->type == 6 ? "on" : "off", ev->data.note.channel + 1,
                    ev->data.note.note, ev->data.note.velocity);
        else if (ev->type == 10)
            fprintf(f, "%.4f %s cc %d %u %d\n", fa_now, port, ev->data.control.channel + 1, ev->data.control.param, ev->data.control.value);
        else
            fprintf(f, "%.4f %s type%d\n", fa_now, port, ev->type);
    }
    return (int)sizeof(fa_event);
}
int snd_seq_close(void* h) { free(h); return 0; }
int snd_seq_connect_to(void* h, int a, int b, int c) { (void)h; (void)a; (void)b; (void)c; return 0; }
int snd_seq_disconnect_to(void* h, int a, int b, int c) { (void)h; (void)a; (void)b; (void)c; return 0; }
int snd_seq_connect_from(void* h, int a, int b, int c) { (void)h; (void)a; (void)b; (void)c; return 0; }
int snd_seq_disconnect_from(void* h, int a, int b, int c) { (void)h; (void)a; (void)b; (void)c; return 0; }
int snd_seq_event_input(void* h, fa_event** ev) { (void)h; (void)ev; return -11; }
