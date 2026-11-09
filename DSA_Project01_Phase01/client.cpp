// ======================= TIME-TRAVEL DEBUGGER - CLIENT TEMPLATE =======================

// The client never talks to the server directly. It only talks to the load balancer.
// The load balancer wakes the server up if it is down, so the client does not care.
// The first "start" may just take a bit longer.
//
// The network is used ONLY by "start". After the .tdbg file is downloaded,
// next / prv / jmp read that file on the local disk. They never contact the load balancer.
//
// How the pieces fit together:
//   main loop -> readCommand -> runCommand
//   start -> cmdStart()  -> requestDebugFile() (Part A) -> openTdbg() (Part B) -> shows step 1
//   next, prv, jmp -> moveBy() (Part D) -> readSnapshotAt() (Part B) -> printSnapshot() (Part E)

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <sys/socket.h>
#include "helper.h"
using namespace std;

// ---- Constants ----
const char* PROGRAM_NAME = "ttdb";
const char* PROMPT = "ttdb> ";
const int IO_BUFFER_SIZE = 64 * 1024;     // fixed buffer for streaming files to and from the socket
const char* TDBG_FILE = "ttdb_last.tdbg"; // where the downloaded .tdbg is saved
const int MAX_JUMP = 1000000000;          // biggest n accepted by jmp
string LB_HOST = "127.0.0.0";
int LB_PORT = 5000;

// These two must be the same as in Phase 1

const int MAX_VARS_PER_FRAME = 16;
const int MAX_STACK_DEPTH = 64;


// ---- Structs (same as Phase 1, so the file can be read back exactly as it was written) ----
struct Variable
{
    string name;
    int32_t value;
};
struct Frame
{
    string func_name;
    int32_t argc;
    Variable argv[MAX_VARS_PER_FRAME];
    int32_t returnLine;
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};

// ---- Structs only the client needs ----
struct TdbgReader
{
    ssize_t file_index;
    TTDBHeader header;
    vector<int64_t> index;
};
struct DebugSession
{
    int active_flag;     // false until a "start" has worked
    TdbgReader reader;   // the .tdbg file stays open for the whole session
    int64_t currentStep;
};

// =============================================================================================
// PART A: TALKING TO THE LOAD BALANCER
// =============================================================================================

bool recvAll(int sock, char* buf, size_t n)
{
    // recv() can return fewer bytes than asked for, so keep calling it until exactly n bytes are read.
    // Return false if the connection closes or fails before n bytes arrive.
    size_t g= 0;
    while (g< n)
    {
        ssize_t r = recv(sock, buf + g, n - g, 0);
        if (r <= 0)
        {
            if (r < 0 && errno == EINTR) 
                continue;
            return false;
        }
        g+= (size_t)r;
    }
    return true;
}
bool sendAll(int sock, const char* buf, size_t n)
{
    // send() can send fewer bytes than asked for, so keep calling it until all n bytes are sent.
    // Use the flag MSG_NOSIGNAL. Return false if the connection fails.
       size_t s = 0;
        while (s< n)
        {
            ssize_t r = send(sock, buf + s, n - s, MSG_NOSIGNAL);
            if (r <= 0)
            {
                if (r < 0 && errno == EINTR) continue;
                return false;
            }
            s+= (size_t)r;
        }
        return true;
    }
int connectToHost(const char* host, int port)
{
    // Create a TCP socket and connect it to host:port.
    // Return the socket, or -1 if the connection fails.
    int s = connectSocket(host, port);
    if (s >= 0)
        setSocketTimeout(s, CLIENT_TIMEOUT_SEC);
    return s;
}
bool uploadSource(int sock, const char* sourcePath)
{
    // have to do checksum and then send
    // When the file is finished, call shutdown(sock, SHUT_WR). This is how the server knows the string is complete.
    // Return false if the file cannot be opened or any send fails.
    FILE* f = fopen(sourcePath, "rb");
    if (!f)
    {
        shutdown(sock, SHUT_WR);
        return false;
    }
    Crc32 crc;
    char status = STATUS_OK;
    crc.update(&status, 1);
    bool ok = sendAll(sock, &status, 1);
    char buf[IO_BUFFER_SIZE];
    while (ok)
    {
        size_t n = fread(buf, 1, sizeof(buf), f);
        if (n == 0)
            break;
        crc.update(buf, n);
        ok = sendAll(sock, buf, n);
    }
    if (ferror(f))
        ok = false;
    fclose(f);
    if (ok)
    {
        char trailer[CRC_SIZE];
        putU32(crc.value(), trailer);
        ok = sendAll(sock, trailer, CRC_SIZE);
    }
    shutdown(sock, SHUT_WR);
    return ok;
}
bool receiveResponse(int sock, const char* outTdbgPath, string& outError)
{
    // Reply on the wire: [status][payload][CRC-32]. The CRC covers the status byte too.
    // Feed every received byte (status included) to a TrailerChecker.
    // Status 1: collect the payload into outError. Status 0: write the payload to the file.
    // At the end call verify(). If it fails, delete the file, set outError to
    // "checksum mismatch, response corrupted" and return false.
    outError = "";
    TrailerChecker chk;
    char b[IO_BUFFER_SIZE];
    char out[IO_BUFFER_SIZE];
    bool gotStatus = false;
    int status = -1;
    FILE* f = nullptr;
    bool lost = false;
    size_t total = 0;
    while (true)
    {
        ssize_t n = recv(sock, b, sizeof(b), 0);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            lost = true;
            break;
        }
        if (n == 0)
            break;
        total += (size_t)n;
        size_t k = chk.feed(b, (size_t)n, out);
        size_t off = 0;
        if (k > 0 && !gotStatus)
        {
            status = (unsigned char)out[0];
            gotStatus = true;
            off = 1;
            if (status == STATUS_OK)
            {
                f = fopen(outTdbgPath, "wb");
                if (!f)
                {
                    outError = "cannot write the .tdbg file";
                    return false;
                }
            }
        }
        if (k > off)
        {
            if (status == STATUS_OK)
                fwrite(out + off, 1, k - off, f);
            else
                outError.append(out + off, k - off);
        }
    }
    if (f) {
        fclose(f);
    }
    if (total == 0)
    {
        outError = "no reply from the load balancer (it may have dropped the request)";
        remove(outTdbgPath);
        return false;
    }
    if (lost)
    {
        outError = "connection to the load balancer was lost";
        remove(outTdbgPath);
        return false;
    }
    if (!gotStatus || !chk.verify())
    {
        remove(outTdbgPath);
        outError = "checksum mismatch, response corrupted";
        return false;
    }
    if (status == STATUS_OK) {
        return true;
    }
    if (status != STATUS_ERROR) {
        outError = "unknown reply status";
    }
    remove(outTdbgPath);
    return false;
}
// =============================================================================================
// PART B: READING THE .tdbg FILE
// File layout (written by Phase 1):
//   header | snapshot 0 | snapshot 1 | ... | index (one 8 byte offset per snapshot)
// The index lets us jump straight to any step without reading the steps before it.
// =============================================================================================
bool openTdbg(const char* path, TdbgReader& r)
{
    r.file = fopen(path, "rb");
    r.index.clear();
    if (!r.file) return false;
    FILE* f = r.file;
    TTDBHeader& h = r.header;
    bool ok = fread(h.magic, 1, 4, f) == 4 &&
        fread(&h.version, sizeof(int32_t), 1, f) == 1 && fread(&h.stepCount, sizeof(int32_t), 1, f) == 1 &&
        fread(&h.indexOffset, sizeof(int64_t), 1, f) == 1;
       if (ok)
        ok = memcmp(h.magic, "TTDB", 4) == 0 && h.version == 1 && h.stepCount > 0;
    if (ok)
    {
        fseek(f, 0, SEEK_END);
        int64_t size = (int64_t)ftell(f);
        ok = h.indexOffset >= TTDB_HEADER_SIZE && h.indexOffset + (int64_t)8 * h.stepCount == size;
    }
    if (ok)
    {
        r.index.resize(h.stepCount);
        fseek(f, (long)h.indexOffset, SEEK_SET);
        ok = fread(&r.index[0], sizeof(int64_t), h.stepCount, f) == (size_t)h.stepCount;
    }
    if (!ok)
    {
        closeTdbg(r);
        return false;
    }
    return true;
}
    
void closeTdbg(TdbgReader& r)
{
    vector<int64_t>().swap(r.index);
    if (r.file)
        fclose(r.file);
    r.file = nullptr;
}
Snapshot* readSnapshotAt(TdbgReader& r, int32_t step)
{
    if (!r.file || step < 0 || step >= r.header.stepCount)
        return nullptr;
    if (fseek(r.file, (long)r.index[step], SEEK_SET) != 0)
        return nullptr;
    Snapshot* s = new Snapshot;
    bool ok = fread(&s->stackDepth, sizeof(int32_t), 1, r.file) == 1 && s->stackDepth >= 0 && s->stackDepth <= MAX_STACK_DEPTH;
        if (!ok)
    {
        delete s;
        return nullptr;
    }
    return s;
}
// =============================================================================================
// PART C: THE SESSION IN MEMORY
// There is one DebugSession variable, created in main() and passed to every command by reference.
// =============================================================================================
void initSession(DebugSession& s)
{
    s.active_flag = 0;
    s.reader.file = nullptr;
    s.currentStep = 0;
}
void closeSession(DebugSession& s)
{
    // If active, call closeTdbg(s.reader). Then set active = false.
    // Called when the user types quit and when the input ends.
    if (s.active_flag)
        closeTdbg(s.reader);
    s.active_flag = 0;
}

// =============================================================================================
// PART D: THE COMMANDS
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s); // defined in Part E

bool showCurrent(DebugSession& s)
{
    // Read the snapshot at s.currentStep, call printSnapshot, delete the snapshot.
    // If readSnapshotAt returns nullptr, print "damaged .tdbg file" and return false.
    Snapshot* snap= readSnapshotAt(s.reader, (int32_t)s.currentStep);
    if (!snap)
    {
        cout << "error file" << endl;
        return false;
    }
    printSnapshot((int32_t)s.currentStep + 1, s.reader.header.stepCount, *snap);
    delete snap;
    return true;
}
bool moveBy(DebugSession& s, int32_t delta)
{
    if (!s.active_flag)
    {
        cout << "no session recently used file>" << endl;
        return false;
    }
    int64_t target = s.currentStep + (int64_t)delta;
    int64_t total = s.reader.header.stepCount;
    if (target < 0 || target >= total)
    {
        if (delta == 1) {
            cout << "already at the last step (" << total << ")" << endl;
        }
        else if (delta == -1) {
            cout << "already at the first step" << endl;
        }
        else {
            cout << " valid steps are 1.." << total << endl;
        }
        return false;
    }
    int64_t old = s.currentStep;
    s.currentStep = target;
    if (!showCurrent(s))
    {
        s.currentStep = old;
        return false;
    }
    return true;
}
bool cmdStart(DebugSession& s, const string& sourcePath)
{
    // 1. requestDebugFile with host and port, saving to TDBG_FILE.
    // 2. If it fails, print the error text and return false. Keep the old session as it was.
    // 3. Open the new file with openTdbg into a NEW local TdbgReader.
    //    If that fails, print "server sent a damaged .tdbg file", keep the old session, return false.
    // 4. Only now close the old reader (if s.active), then put the new reader into s,
    //    set s.currentStep = 0 and s.active = true.
    // 5. Show step 1 with showCurrent(s).
    string e;
    if (!requestDebugFile(LB_HOST.c_str(), LB_PORT, sourcePath.c_str(), TDBG_FILE, e))
    {
        cout << e<< endl;
        return false;
    }
    TdbgReader fr;
    fr.file = nullptr;
    if (!openTdbg(TDBG_FILE, fr))
    {
        cout << "server sent a damaged file" << endl;
        return false;
    }
    if (s.active_flag)
        closeTdbg(s.reader);
    s.reader = fr;
    s.currentStep = 0;
    s.active_flag = 1;
    return showCurrent(s);
}
bool cmdNext(DebugSession& s) {
    return moveBy(s, 1);
}
bool cmdPrv(DebugSession& s) {
    return moveBy(s, -1);
}
bool cmdJmpNext(DebugSession& s, int32_t n) { 
    return moveBy(s, n); 
}
bool cmdJmpPrv(DebugSession& s, int32_t n) { 
    return moveBy(s, -n);
}


// =============================================================================================
// PART E: PRINTING
// =============================================================================================

void printSnapshot(int32_t step, int32_t totalSteps, const Snapshot& s)
{
    cout << "Step " << step << totalSteps << " ===" << endl;
    cout << "stack:" << endl;
    for (int32_t i = 0; i < s.stackDepth; i++)
    {
        const Frame& fr = s.callStack[i];
        cout  << i << " " << fr.func_name << endl;
    }
}
void printHelp()
{
    // Print the commands
    cout << "Commands:" << endl;
    cout << "  start <source file>      send the program to the server and show step 1" << endl;
    cout << "  next          go one step forward" << endl;
    cout << "  prv            go one step back" << endl;
    cout << "  jmp next <n>        go n steps forward" << endl;
    cout << "  jmp prv <n>        go n steps back" << endl;
    cout << "  help            show this text" << endl;
    cout << "  quit | exit      leave" << endl;
}

// =============================================================================================
// PART F: THE INFINITE LOOP
// =============================================================================================

bool runCommand(DebugSession& s, const string& line, bool& quitRequested)
{
    // 1. Split the line into words.
    // 2. If the first word is "ttdb", drop it.
    // 3. Match the first word:
    //      start      -> needs a path after it, then cmdStart
    //      next       -> no more words, then cmdNext
    //      prv        -> no more words, then cmdPrv
    //      jmp        -> second word "next" or "prv", third word a positive number ,
    //                    then cmdJmpNext or cmdJmpPrv
    //      help       -> printHelp
    //      quit, exit -> set quitRequested = true
    // 4. For anything else, or a wrong number of words, print "unknown command" and the help text, and return false.
    vector<string> w;
    istringstream iss(line);
    string word;
    while (iss >> word) w.push_back(word);
    if (!w.empty() && w[0] == PROGRAM_NAME) w.erase(w.begin());
    if (w.empty()) return true;

    const string& cmd = w[0];
    if (cmd == "start" && w.size() == 2) {
        return cmdStart(s, w[1]);
    }
    if (cmd == "next" && w.size() == 1) {
        return cmdNext(s);
    }
    if (cmd == "prv" && w.size() == 1) {
        return cmdPrv(s);
    }
    if (cmd == "help" && w.size() == 1) { 
        printHelp();
        return true; 
    }
    if ((cmd == "quit" || cmd == "exit") && w.size() == 1) { 
        quitRequested = true; 
        return true;
    }

    cout << "unknown command" << endl;
    printHelp();
    return false;
}
int main(int argc, char* argv[])
{
    // you get string LB_HOST and int LB_PORT = 0 in arguments of this main function. 

    // DebugSession session; initSession(session);
    // Print a short welcome and the help text.
    //
    // Infinite loop:
    //   if quitRequested, break
    //
    // After the loop: closeSession(session) and return 0.
    // Never exit the loop because of an error. Only quit or the end of the input leaves it.
    if (argc >= 2) LB_HOST = argv[1];
    if (argc >= 3) LB_PORT = atoi(argv[2]);

    DebugSession session;
    initSession(session);
    cout << "Time-Travel Debugger client, load balancer at " << LB_HOST << ":" << LB_PORT << endl;
    printHelp();

    bool quitRequested = false;
    string line;
    while (!quitRequested)
    {
        cout << PROMPT << flush;
        if (!getline(cin, line)) break;
        runCommand(session, line, quitRequested);
    }
    closeSession(session);
    return 0;

}