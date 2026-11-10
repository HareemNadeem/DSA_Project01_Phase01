// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <cstring>
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "helper.h"
using namespace std;

// ---- Constants & Configurations ----
const string LB_HOST = "127.0.0.1";
const int LB_PORT = 5000;
const char* TDBG_FILE = "session.tdbg";

// ---- Client TdbgReader & DebugSession Structures ----
struct TdbgReader
{
    FILE* file;
    TTDBHeader header;
    int64_t* index;
};

struct DebugSession
{
    TdbgReader reader;
    int32_t currentStep;
    bool active;
};

// ---- Helper Declaration Forwarding ----
void closeTdbg(TdbgReader& r);

// ---- Reading TDBG File Helpers ----
bool openTdbg(const char* path, TdbgReader& r)
{
    r.file = fopen(path, "rb");
    if (!r.file) return false;

    if (fread(r.header.magic, 1, 4, r.file) != 4) {
        closeTdbg(r);
        return false;
    }
    if (fread(&r.header.version, sizeof(int32_t), 1, r.file) != 1 ||
        fread(&r.header.stepCount, sizeof(int32_t), 1, r.file) != 1 ||
        fread(&r.header.indexOffset, sizeof(int64_t), 1, r.file) != 1) {
        closeTdbg(r);
        return false;
    }

    if (r.header.stepCount <= 0) {
        closeTdbg(r);
        return false;
    }

    r.index = new int64_t[r.header.stepCount];
    if (fseek(r.file, (long)r.header.indexOffset, SEEK_SET) != 0) {
        closeTdbg(r);
        return false;
    }

    if (fread(r.index, sizeof(int64_t), r.header.stepCount, r.file) != (size_t)r.header.stepCount) {
        closeTdbg(r);
        return false;
    }

    return true;
}

void closeTdbg(TdbgReader& r)
{
    if (r.file) {
        fclose(r.file);
        r.file = nullptr;
    }
    if (r.index) {
        delete[] r.index;
        r.index = nullptr;
    }
}

// Deserialization Helpers
string read_String(FILE* f)
{
    int32_t len = 0;
    if (fread(&len, sizeof(int32_t), 1, f) != 1 || len < 0) return "";
    if (len == 0) return "";
    string s(len, '\0');
    if (fread(&s[0], 1, len, f) != (size_t)len) return "";
    return s;
}

void read_Variable(FILE* f, Variable& v)
{
    v.name = read_String(f);
    if (fread(&v.value, sizeof(int32_t), 1, f) != 1) {
        v.value = 0;
    }
}

void read_Frame(FILE* f, Frame& fr)
{
    fr.func_name = read_String(f);
    if (fread(&fr.argc, sizeof(int32_t), 1, f) != 1) fr.argc = 0;
    for (int32_t i = 0; i < fr.argc && i < MAX_VARS_PER_FRAME; i++) {
        read_Variable(f, fr.argv[i]);
    }
    if (fread(&fr.returnLine, sizeof(int32_t), 1, f) != 1) fr.returnLine = -1;
    if (fread(&fr.localCount, sizeof(int32_t), 1, f) != 1) fr.localCount = 0;
    for (int32_t i = 0; i < fr.localCount && i < MAX_VARS_PER_FRAME; i++) {
        read_Variable(f, fr.locals[i]);
    }
}

Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (!r.file || step < 0 || step >= r.header.stepCount) return nullptr;
    if (fseek(r.file, (long)r.index[step], SEEK_SET) != 0) return nullptr;

    Snapshot* s = new Snapshot();
    bool ok = fread(&s->stackDepth, sizeof(int32_t), 1, r.file) == 1 &&
        s->stackDepth >= 0 && s->stackDepth <= MAX_STACK_DEPTH;
    if (!ok) {
        delete s;
        return nullptr;
    }

    for (int32_t i = 0; i < s->stackDepth; i++) {
        read_Frame(r.file, s->callStack[i]);
    }
    return s;
}

// ---- Network Helper to request debug file from Load Balancer ----
bool requestDebugFile(const char* host, int port, const char* sourcePath, const char* outTdbgPath, string& errorOut)
{
    ifstream src(sourcePath, ios::binary);
    if (!src) {
        errorOut = "Cannot open source file: " + string(sourcePath);
        return false;
    }

    string payload((istreambuf_iterator<char>(src)), istreambuf_iterator<char>());
    src.close();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        errorOut = "Failed to create socket.";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        errorOut = "Failed to connect to Load Balancer.";
        return false;
    }

    if (!sendAll(sock, payload.c_str(), payload.length())) {
        close(sock);
        errorOut = "Failed to send trace data.";
        return false;
    }

    shutdown(sock, SHUT_WR);

    char status = 0;
    if (recv(sock, &status, 1, 0) <= 0) {
        close(sock);
        errorOut = "Server response empty.";
        return false;
    }

    if (status != 0) {
        char errBuf[1024];
        ssize_t n = recv(sock, errBuf, sizeof(errBuf) - 1, 0);
        if (n > 0) {
            errBuf[n] = '\0';
            errorOut = string(errBuf);
        }
        else {
            errorOut = "Server returned error status.";
        }
        close(sock);
        return false;
    }

    FILE* outF = fopen(outTdbgPath, "wb");
    if (!outF) {
        close(sock);
        errorOut = "Cannot open output file to write .tdbg.";
        return false;
    }

    char buf[64 * 1024];
    ssize_t nRead;
    while ((nRead = recv(sock, buf, sizeof(buf), 0)) > 0) {
        fwrite(buf, 1, nRead, outF);
    }
    fclose(outF);
    close(sock);
    return true;
}

// ---- Session Controls ----
void initSession(DebugSession& s)
{
    s.reader.file = nullptr;
    s.reader.index = nullptr;
    s.reader.header.stepCount = 0;
    s.currentStep = 0;
    s.active = false;
}

void printSnapshot(const Snapshot* ss, int32_t step, int32_t totalSteps)
{
    if (!ss) return;
    cout << "=== Step " << step << " / " << (totalSteps - 1) << " ===" << endl;
    cout << "Call Stack (Depth: " << ss->stackDepth << "):" << endl;
    for (int32_t i = ss->stackDepth - 1; i >= 0; i--) {
        const Frame& f = ss->callStack[i];
        cout << "  [" << i << "] " << f.func_name << "(";
        for (int32_t a = 0; a < f.argc; a++) {
            cout << f.argv[a].name << "=" << f.argv[a].value;
            if (a < f.argc - 1) cout << ", ";
        }
        cout << ")";
        if (f.returnLine != -1) cout << " retLine=" << f.returnLine;
        cout << endl;

        if (f.localCount > 0) {
            cout << "      Locals: ";
            for (int32_t l = 0; l < f.localCount; l++) {
                cout << f.locals[l].name << "=" << f.locals[l].value;
                if (l < f.localCount - 1) cout << ", ";
            }
            cout << endl;
        }
    }
}

bool cmdStart(DebugSession& s, const string& sourcePath)
{
    if (s.active) {
        closeTdbg(s.reader);
        s.active = false;
    }

    string err;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), TDBG_FILE, err)) {
        cout << "Error starting debug session: " << err << endl;
        return false;
    }

    TdbgReader fr;
    fr.file = nullptr;
    fr.index = nullptr;
    if (!openTdbg(TDBG_FILE, fr)) {
        cout << "Error reading generated .tdbg file." << endl;
        return false;
    }

    s.reader = fr;
    s.currentStep = 0;
    s.active = true;

    Snapshot* ss = readSnapshotAt(s.reader, s.currentStep);
    printSnapshot(ss, s.currentStep, s.reader.header.stepCount);
    delete ss;
    return true;
}

bool cmdNext(DebugSession& s)
{
    if (!s.active) {
        cout << "No active debug session. Use 'start <trace_file>' first." << endl;
        return false;
    }
    if (s.currentStep + 1 >= s.reader.header.stepCount) {
        cout << "Already at the end of execution timeline." << endl;
        return false;
    }
    s.currentStep++;
    Snapshot* ss = readSnapshotAt(s.reader, s.currentStep);
    printSnapshot(ss, s.currentStep, s.reader.header.stepCount);
    delete ss;
    return true;
}

bool cmdPrv(DebugSession& s)
{
    if (!s.active) {
        cout << "No active debug session. Use 'start <trace_file>' first." << endl;
        return false;
    }
    if (s.currentStep <= 0) {
        cout << "Already at the beginning of execution timeline." << endl;
        return false;
    }
    s.currentStep--;
    Snapshot* ss = readSnapshotAt(s.reader, s.currentStep);
    printSnapshot(ss, s.currentStep, s.reader.header.stepCount);
    delete ss;
    return true;
}

bool cmdJmpNext(DebugSession& s)
{
    if (!s.active) return false;
    return cmdNext(s);
}

bool cmdJmpPrv(DebugSession& s)
{
    if (!s.active) return false;
    return cmdPrv(s);
}

// ---- Command Processing Loop ----
bool runCommand(DebugSession& s, const string& line, bool& quit)
{
    istringstream iss(line);
    string cmd;
    iss >> cmd;

    if (cmd == "quit" || cmd == "exit") {
        quit = true;
        return true;
    }
    else if (cmd == "start") {
        string traceFile;
        iss >> traceFile;
        if (traceFile.empty()) {
            cout << "Usage: start <trace_file>" << endl;
            return false;
        }
        return cmdStart(s, traceFile);
    }
    else if (cmd == "step" || cmd == "next") {
        return cmdNext(s);
    }
    else if (cmd == "prev" || cmd == "prv") {
        return cmdPrv(s);
    }
    else if (cmd == "jmpnext") {
        return cmdJmpNext(s);
    }
    else if (cmd == "jmpprv") {
        return cmdJmpPrv(s);
    }
    else if (!cmd.empty()) {
        cout << "Unknown command: " << cmd << endl;
    }
    return true;
}

int main()
{
    DebugSession session;
    initSession(session);

    cout << "Time-Travel Debugger Client Ready." << endl;
    cout << "Commands: start <file>, step, prev, jmpnext, jmpprv, quit" << endl;

    string line;
    bool quit = false;
    while (!quit) {
        cout << "ttdb> ";
        if (!getline(cin, line)) break;
        runCommand(session, line, quit);
    }

    if (session.active) {
        closeTdbg(session.reader);
    }
    cout << "Client exited." << endl;
    return 0;
}