// alsa_out.h - the plugin's own ALSA sequencer client: lights to the LinnStrument, and an
// optional accompaniment port for MPC tracks.
//
// Adapted from mpc-mpe's shared/mpe/alsa_seq.h (MIT, same author). libasound is loaded with
// dlopen(), so the binary needs no ALSA headers or link flags and runs without ports where
// there is no ALSA (a computer, the host simulator).
//
// Two ports:
//   "Linn Out"  READ | NO_EXPORT. Nobody else can subscribe to it, so its messages reach only
//               the LinnStrument, never an MPC track listening to "All Ports" (where guide
//               note-ons would play sounds). This client subscribes it to the LinnStrument's
//               port itself: the kernel's USB MIDI client opens its output only for a
//               subscriber, so a bare direct event could be dropped.
//   "MIDI Out"  READ | SUBS_READ. The MPC lists it as a track input ("Sight Reader MIDI Out");
//               the other hand's notes go out here when Accomp is on.
//   "Linn In"   WRITE | NO_EXPORT, on a second, input-only client (0.5.5). Subscribed from the
//               LinnStrument's port, so the instrument's answers to setting requests (its
//               Octave and Transpose, NRPN 299) reach the plugin whatever the MPC routes. Read
//               on the worker thread, so the audio thread's output handle is never shared.
//
// Threading: open/close and find/connect on the worker thread; send on the audio thread
// (non-blocking system call, no allocation, no locks).
//
// SPDX-License-Identifier: MIT
#ifndef SR_ALSA_OUT_H
#define SR_ALSA_OUT_H

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#if defined(__linux__)
  #include <dlfcn.h>
  #define SR_HAVE_DLOPEN 1
#else
  #define SR_HAVE_DLOPEN 0
#endif

namespace sr {

struct SeqAddr { uint8_t client, port; };
struct SeqEvNote { uint8_t channel, note, velocity, offVelocity; uint32_t duration; };
struct SeqEvCtrl { uint8_t channel, unused[3]; uint32_t param; int32_t value; };
struct SeqEvent
{
    uint8_t type, flags, tag, queue;
    uint32_t time[2];
    SeqAddr source, dest;
    union { SeqEvNote note; SeqEvCtrl control; uint8_t raw[12]; } data;
};
static_assert(sizeof(SeqEvent) == 28, "snd_seq_event must be 28 bytes");

enum : uint8_t { kSeqNoteOn = 6, kSeqNoteOff = 7, kSeqController = 10, kSeqNonRegParam = 15, kSeqQueueDirect = 253, kSeqAddrSubscribers = 254 };
enum : unsigned
{
    kSeqCapRead = 1u << 0, kSeqCapWrite = 1u << 1, kSeqCapSubsRead = 1u << 5, kSeqCapSubsWrite = 1u << 6,
    kSeqCapNoExport = 1u << 7, kSeqTypeMidiGeneric = 1u << 1, kSeqTypeApplication = 1u << 20,
    kSeqOpenOutput = 1, kSeqOpenInput = 2, kSeqOpenDuplex = 3, kSeqNonblock = 1
};

class AlsaOut
{
public:
    ~AlsaOut() { close(); }

    bool open(const char* clientName)
    {
        if (seq_) return true;
        if (!api().load()) return false;
        void* s = nullptr;
        if (api().open(&s, "default", kSeqOpenDuplex, kSeqNonblock) < 0 || !s) return false;
        api().setName(s, clientName);
        linnPort_ = api().createPort(s, "Linn Out", kSeqCapRead | kSeqCapNoExport, kSeqTypeMidiGeneric | kSeqTypeApplication);
        midiPort_ = api().createPort(s, "MIDI Out", kSeqCapRead | kSeqCapSubsRead, kSeqTypeMidiGeneric | kSeqTypeApplication);
        seq_ = s;
        // the input side, for the LinnStrument's answers
        void* in = nullptr;
        if (api().connectFrom && api().eventInput && api().open(&in, "default", kSeqOpenInput, kSeqNonblock) >= 0 && in)
        {
            api().setName(in, "Sight Reader Linn In");
            inPort_ = api().createPort(in, "Linn In", kSeqCapWrite | kSeqCapNoExport, kSeqTypeMidiGeneric | kSeqTypeApplication);
            if (inPort_ >= 0) seqIn_ = in; else api().close(in);
        }
        return true;
    }
    void close()
    {
        if (seqIn_) { api().close(seqIn_); seqIn_ = nullptr; }
        if (seq_) { api().close(seq_); seq_ = nullptr; }
        linnPort_ = midiPort_ = inPort_ = -1; linnClient_ = -1; inClient_ = -1; connected_.store(false);
    }
    // Worker thread: the NRPN values the LinnStrument has sent since the last call,
    // f(parameter, value). Controller events are put together per channel (99/98 number,
    // 6/38 value); a sequencer that already did it hands over NRPN events.
    template <typename F> void pollLinnNrpn(F f)
    {
        if (!seqIn_) return;
        for (int guard = 0; guard < 512; ++guard)
        {
            SeqEvent* ev = nullptr;
            if (api().eventInput(seqIn_, &ev) < 0 || !ev) break;
            if (ev->type == kSeqNonRegParam) { f((int)ev->data.control.param, (int)ev->data.control.value); continue; }
            if (ev->type != kSeqController) continue;
            Nrpn& n = nrpn_[ev->data.control.channel & 15];
            const int v = ev->data.control.value & 127;
            switch (ev->data.control.param)
            {
                case 99: n.msb = v; break;
                case 98: n.lsb = v; break;
                case 6:  n.dataMsb = v; break;
                case 38: if (n.msb != 127 || n.lsb != 127) f((n.msb << 7) | n.lsb, (n.dataMsb << 7) | v); break;
                default: break;
            }
        }
    }
    bool isOpen() const { return seq_ != nullptr; }

    // Finds where the light messages go (from /proc/asound/seq/clients) and subscribes Linn Out
    // to it: mode 0 the LinnStrument's own USB port (a client named "...LinnStrument..."),
    // 1 and 2 the MPC's MIDI Out A and B (hardware ports named "DIN-A" / "DIN-B"), for a
    // LinnStrument wired by DIN. Worker thread. Returns true while connected.
    bool findAndConnectLinn(int mode = 0)
    {
        if (!seq_ || linnPort_ < 0) return false;
        int client = -1, port = -1;
        bool found = mode == 0 ? scanProc("linnstrument", true, client, port)
                               : scanProc(mode == 1 ? "din-a" : "din-b", false, client, port);
        if (connected_.load() && found && client == linnClient_ && port == linnDestPort_) return true;
        if (connected_.load())
        {   // the place changed (or went away): let go of the old one first
            if (api().disconnectTo) api().disconnectTo(seq_, linnPort_, linnClient_, linnDestPort_);
            connected_.store(false); linnClient_ = -1; linnDestPort_ = -1;
        }
        if (!found) return false;
        if (api().connectTo && api().connectTo(seq_, linnPort_, client, port) >= 0)
        {
            linnClient_ = client; linnDestPort_ = port;
            linnGen_.fetch_add(1);
            connected_.store(true);
            // listen to the same port for the instrument's answers (a DIN port may not be readable)
            if (seqIn_ && inPort_ >= 0)
            {
                if (inClient_ >= 0 && api().disconnectFrom) api().disconnectFrom(seqIn_, inPort_, inClient_, inSrcPort_);
                inClient_ = -1;
                if (api().connectFrom(seqIn_, inPort_, client, port) >= 0) { inClient_ = client; inSrcPort_ = port; }
            }
            return true;
        }
        return false;
    }
    int linnClient() const { return linnClient_; }
    int linnDestPort() const { return linnDestPort_; }
    bool linnConnected() const { return connected_.load(); }
    // Goes up by one on every new connection (another port, or the instrument plugged back in),
    // so the audio thread knows to repaint.
    int linnGeneration() const { return linnGen_.load(); }

    // Audio thread: one channel message to the LinnStrument or the MIDI Out port.
    bool sendLinn(const uint8_t* m) { return connected_.load() ? sendFrom(linnPort_, m) : false; }
    bool sendMidi(const uint8_t* m) { return midiPort_ >= 0 ? sendFrom(midiPort_, m) : false; }

private:
    bool sendFrom(int port, const uint8_t* m)
    {
        if (!seq_ || port < 0) return false;
        SeqEvent ev; std::memset(&ev, 0, sizeof(ev));
        uint8_t st = m[0] & 0xF0, ch = m[0] & 0x0F;
        if (st == 0x90 && m[2] > 0) { ev.type = kSeqNoteOn; ev.data.note.channel = ch; ev.data.note.note = m[1]; ev.data.note.velocity = m[2]; }
        else if (st == 0x80 || st == 0x90) { ev.type = kSeqNoteOff; ev.data.note.channel = ch; ev.data.note.note = m[1]; ev.data.note.velocity = 0; }
        else if (st == 0xB0) { ev.type = kSeqController; ev.data.control.channel = ch; ev.data.control.param = m[1]; ev.data.control.value = m[2]; }
        else return false;
        ev.queue = kSeqQueueDirect;
        ev.source.port = (uint8_t)port;
        ev.dest.client = kSeqAddrSubscribers; ev.dest.port = 0;
        return api().outputDirect(seq_, &ev) >= 0;
    }

    // byClient: the needle is in the client's name (its first writable port is taken); else in
    // a port's name. Only kernel clients (hardware) count, so the MPC application's own mirror
    // ports ("MPC LIVE III DIN-A" in client "MPC 3") are never taken for the device.
    static bool scanProc(const char* needle, bool byClient, int& client, int& port)
    {
        FILE* f = std::fopen("/proc/asound/seq/clients", "r");
        if (!f) return false;
        auto lower = [](const char* s) { std::string n(s); for (char& ch : n) ch = (char)((ch >= 'A' && ch <= 'Z') ? ch + 32 : ch); return n; };
        char line[256]; int curClient = -1; bool kernel = false, match = false; bool found = false;
        while (std::fgets(line, sizeof(line), f))
        {
            int c; char name[200];
            if (std::sscanf(line, "Client %d : \"%199[^\"]\"", &c, name) == 2)
            {
                curClient = c;
                kernel = std::strstr(line, "[Kernel") != nullptr;
                match = byClient && lower(name).find(needle) != std::string::npos;
                continue;
            }
            int p; char pname[200]; char caps[16];
            if (kernel && std::sscanf(line, " Port %d : \"%199[^\"]\" (%15[^)])", &p, pname, caps) == 3)
            {
                bool writable = std::strchr(caps, 'W') != nullptr;
                if (writable && (match || (!byClient && lower(pname).find(needle) != std::string::npos)))
                { client = curClient; port = p; found = true; break; }
            }
        }
        std::fclose(f);
        return found;
    }

    struct Api
    {
        int state = 0;
        int (*open)(void**, const char*, int, int) = nullptr;
        int (*setName)(void*, const char*) = nullptr;
        int (*createPort)(void*, const char*, unsigned, unsigned) = nullptr;
        int (*outputDirect)(void*, SeqEvent*) = nullptr;
        int (*close)(void*) = nullptr;
        int (*connectTo)(void*, int, int, int) = nullptr;
        int (*disconnectTo)(void*, int, int, int) = nullptr;
        int (*connectFrom)(void*, int, int, int) = nullptr;
        int (*disconnectFrom)(void*, int, int, int) = nullptr;
        int (*eventInput)(void*, SeqEvent**) = nullptr;
        bool load()
        {
            if (state != 0) return state > 0;
            state = -1;
#if SR_HAVE_DLOPEN
            void* h = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
            if (!h) return false;
            *(void**)(&open)         = dlsym(h, "snd_seq_open");
            *(void**)(&setName)      = dlsym(h, "snd_seq_set_client_name");
            *(void**)(&createPort)   = dlsym(h, "snd_seq_create_simple_port");
            *(void**)(&outputDirect) = dlsym(h, "snd_seq_event_output_direct");
            *(void**)(&close)        = dlsym(h, "snd_seq_close");
            *(void**)(&connectTo)    = dlsym(h, "snd_seq_connect_to");
            *(void**)(&disconnectTo) = dlsym(h, "snd_seq_disconnect_to");
            *(void**)(&connectFrom)  = dlsym(h, "snd_seq_connect_from");
            *(void**)(&disconnectFrom) = dlsym(h, "snd_seq_disconnect_from");
            *(void**)(&eventInput)   = dlsym(h, "snd_seq_event_input");
            if (open && setName && createPort && outputDirect && close) state = 1;
#endif
            return state > 0;
        }
    };
    static Api& api() { static Api a; return a; }

    void* seq_ = nullptr;
    void* seqIn_ = nullptr;
    int inPort_ = -1, inClient_ = -1, inSrcPort_ = -1;
    struct Nrpn { int msb = 127, lsb = 127, dataMsb = 0; };
    Nrpn nrpn_[16];
    int linnPort_ = -1, midiPort_ = -1;
    int linnClient_ = -1, linnDestPort_ = -1;
    std::atomic<bool> connected_{false};
    std::atomic<int> linnGen_{0};
};

} // namespace sr
#endif
