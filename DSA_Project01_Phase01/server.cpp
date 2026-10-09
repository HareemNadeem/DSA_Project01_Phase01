// ======================= TIME-TRAVEL DEBUGGER - SERVER TEMPLATE =======================

// Pipeline this file implements, top to bottom:
//   0. Receive  -- stream the client's .trace bytes straight to source.bin on disk
//   1. Pass 0X0   -- validity check (FUNC/FUNC_END matching)
//   2. Pass 0X1   -- resolve(): copy EVERY source line into resolve.bin as [offset][size][string], then patch CALL targets.
//   3. Pass 0X2   -- execute resolve.bin: tokenize ONE line at a time, update the call stack, take a snapshot -> Timeline
//   4. Pass 0X3   -- serialize Timeline -> session.tdbg(header + snapshot records + dense index)
#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <cstdint>
#include <fstream>
#include <cstdio>
using namespace std;
// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // kW + func_name + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap on the declared file length
const int32_t IO_BUFFER_SIZE = 64 * 1024;                  // fixed buffer for streaming to/from disk
const int32_t SOCKET_TIMEOUT_SEC = 5;                      // TODO: apply as SO_RCVTIMEO so a deadclient can't hang the server forever

// ---- Custom data structures

// Stack: back the live Call Stack during execution
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* top;
    int32_t count;

public:
    // Implement these functions:
    Stack()
    { // initialize the stack
        top = nullptr;
        count = 0;
    }
    void push(const T& val)
    {

        // pushes the value on the stack if max limit is not reached yet.
        if (count >= MAX_STACK_DEPTH)
            return;
        Node* num = new Node;
        num->data = val;
        num->next = top;
        top = num;
        count++;
    }
    T pop()
    {
        // pop the top value on the stack
        if (top == nullptr)
            return T();
        Node* temp = top;
        T val = temp->data;
        top = top->next;
        delete temp;
        count--;
        return val;
    }
    T& peek()
    {
        // returns the top value on the stack
        return top->data;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int32_t depth()
    {
        return count;
    }
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        // copies every frame, top to bottom in the array given as a parameter
        // this is what buildSnapshot() call, returns count written
        int32_t copi = 0;
        Node* curr = top;
        while (curr != nullptr && copi < maxLen)
        {
            out[copi] = curr->data;
            copi++;
            curr = curr->next;
        }
        return copi;
    }
    //////destructor was missing so i made it to avoid mwmory leak~
    ~Stack() {
        if (!isEmpty()) {
            pop();
        }
    }
};
// Timeline : doubly linked list of Snapshots
struct Snapshot; // fwd declaration;
struct TimelineNode
{
    Snapshot *data;
    TimelineNode *next;
    TimelineNode *prev;
};
class Timeline
{
    TimelineNode *head, *tail;
    int32_t stepCount;

public:
    // Implement these functions
    Timeline(){
       head = nullptr;
        tail = nullptr;
       stepCount = 0;
    }
    ~Timeline();
    void record(Snapshot *s){
         // add record in the timeline
        TimelineNode* num = new TimelineNode;
        num->data = s;
        num->next = nullptr;
        num->prev = tail;
        if (tail != nullptr) {
            tail->next = num;
        }
        else {
                        head = num;
        }
        tail = num;
        stepCount++;
    }

    TimelineNode* begin(){
        return head;
    }
    int32_t getStepCount() {
        return stepCount;
    }
  };
Timeline::~Timeline() {
    TimelineNode* curr = head;
    while (curr != nullptr)
    {
        TimelineNode* neww = curr->next;
        delete curr->data;
        delete curr;
        curr = neww;
    }
}
// Core structs
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
/////////////////////

////////////////////////
struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
void writeHeader(FILE *f, const TTDBHeader &ttb){
    fwrite(ttb.magic, 1, 4, f);
    fwrite(&ttb.version, sizeof(int32_t), 1, f);
    fwrite(&ttb.stepCount, sizeof(int32_t), 1, f);
    fwrite(&ttb.indexOffset, sizeof(int64_t), 1, f);
    // placeholder for other two data members
}
//helping func for this part
void wrt_String(FILE* f, const string& s)
{
    int32_t length = (int32_t)s.size();///fixed size LIKLE 4 bytes for length of string
    fwrite(&length, sizeof(int32_t), 1, f);
    if (length > 0)
        fwrite(s.c_str(), 1, length, f);
}
//////// contains writestring one 
void wrt_Variable(FILE* f, const Variable& var)
{
    wrt_String(f, var.name);
    fwrite(&var.value, sizeof(int32_t), 1, f);
}
///////////contains writevariable one and writevariable 
void wrt_Frame(FILE* f, const Frame& fr)
{
    wrt_String(f, fr.func_name);
    fwrite(&fr.argc, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < fr.argc; i++)
        wrt_Variable(f, fr.argv[i]);
    fwrite(&fr.returnLine, sizeof(int32_t), 1, f);
    fwrite(&fr.localCount, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < fr.localCount; i++)
        wrt_Variable(f, fr.locals[i]);
}
////////// contains frame 
void wrt_Snapshot(FILE* f, const Snapshot& ss)
{
    fwrite(&ss.stackDepth, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < ss.stackDepth; i++)
        wrt_Frame(f, ss.callStack[i]);
}
// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // where this function's FUNC header record sits
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // where in resolve.bin to seek back and overwrite
    string targetFuncName;
};
///helper func
bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}
// PASS 0x0: READING source.bin + VALIDITY CHECK
bool readSourceLine(ifstream &in, string &out)
{
    // reads the next nonblank line
// 1. Read non-blank line
         string line;
        while (getline(in, line)) {
            int start = 0;
            int len = (int)line.length();
       while (start < len && isSpace(line[start])) {
                start++;
            }

            int end = len;
            while (end > start && isSpace(line[end - 1])) {
                end--;
            }

            if (start < end) {
                out = line.substr(start, end - start);
                return true;
            }
        }
        return false;
    }
string firstWord(const string &line)
{
    // returns first word from the input string
    string word = "";
    for (int i = 0; i < line.length(); i++) {
        if (line[i] == ' ' || line[i] == '\t') {
            break;
        }
        word += line[i];
    }
    return word;
}
string secondWord(const string &line)
{
    int i = 0;
        while (i < line.length() && line[i] != ' ' && line[i] != '\t') {
        i++;
    }
    while (i < line.length() && (line[i] == ' ' || line[i] == '\t')) {
        i++;
    }
    string word = "";
    while (i < line.length() && line[i] != ' ' && line[i] != '\t') {
        word += line[i];
        i++;
    }
    return word;
}
string Lower(const string& s)
{
    string r = s;
    for (size_t i = 0; i < r.size(); i++)
    {
        if (r[i] >= 'A' && r[i] <= 'Z')
            r[i] = (char)(r[i] - 'A' + 'a');
    }
    return r;
}
bool validateProgram(const char *sourcePath)
{
    // for each func defined there should be exactly one func_end and no nested funcs allowed - 
    ifstream in(sourcePath, ios::binary);
    if (!in) return false;
        bool inside = false;
    string line;
        while (readSourceLine(in, line))
    {
        string word = Lower(firstWord(line));

        if (word == "func") {
            if (inside) return false;
            inside = true;
        }
        else if (word == "func_end") {
            if (!inside) {
                return false;
            }
            inside = false;
        }
    }

    return !inside;
}

// PASS 0x1: RESOLVE() -> resolve.bin
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    // writes one [offset(8B)][size(4B)][string] record at the current file position
    // returns this record's own starting byte position
    int64_t pos = (int64_t)ftell(f);
    int32_t size = (int32_t)text.size();
        fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    if (size > 0) {
        fwrite(text.c_str(), 1, size, f);
    }
    return pos;
}
int64_t readResolveRecord(FILE *f, string &outText)
{
    // reads one record at the current position and advances past it, returns the offset field - the raw line text comes back untouched in outText.
    int64_t off;
    int32_t size;
    if (fread(&off, sizeof(int64_t), 1, f) != 1)
        return -1;
    if (fread(&size, sizeof(int32_t), 1, f) != 1)
        return -1;
    outText.clear();
    if (size > 0)
    {
        outText.resize(size);
        if (fread(&outText[0], 1, size, f) != (size_t)size)
            return -1;
    }
    return off;
}
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;
    // Every source line becomes one record holding the raw line, as-is.
    ifstream in(sourcePath, ios::binary);
    if (!in)
    {
        cerr << "ERROR" << sourcePath << endl;
        return -1;
    }
       FILE* out = fopen(resolveBinPath, "wb+");
    if (!out)
    {
        cerr << "ERROR" << resolveBinPath << endl;
        return -1;
    }
    // resolve() only PEEKS at the leading word(s) -- enough to spot FUNC
    bool yes= true;
    string line;
    while (yes&& readSourceLine(in, line))
    {
        string k = Lower(firstWord(line));
        // Pehle record likho (offset field = apni position), position wapas milti hai
        int64_t pos = writeResolveRecord(out, (int64_t)ftell(out), line);

        if (k== "func")
        {
            string name = secondWord(line);
            if (funcCount >= MAX_FUNCS)
            {
                cerr << "ERROR: too many functions (max " << MAX_FUNCS << ")" << endl;
                yes= false;
                break;
            }
            for (int32_t i = 0; i < funcCount; i++) // duplicate naam check
            {
                if (funcArray[i].funcName == name)
                {
                    cerr << "ERROR: function '" << name << "' defined more than once" << endl;
                    yes= false;
                    break;
                }
            }
            if (!yes)
                break;
            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;
        }
        else if (k== "call")
        {
            if (patchCount >= MAX_PATCHES)
            {
                cerr << "ERROR "<< MAX_PATCHES << endl;
                yes= false;
                break;
            }
            patches[patchCount].byteOffsetOfOffsetField = pos; 
            patches[patchCount].targetFuncName = secondWord(line);
            patchCount++;
        }
    }
    // (remember its position) and CALL (remember which function it needs
    // and where its offset field sits).
    for (int32_t i = 0; yes&& i < patchCount; i++)
    {
        int64_t target = -1;
        for (int32_t j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == patches[i].targetFuncName)
            {
                target = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if (target < 0)
        {
            cerr << "ERROR" << patches[i].targetFuncName << endl;
            yes=false;
            break;
        }
        fseek(out, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET); 
        fwrite(&target, sizeof(int64_t), 1, out);                  
    }
    // Once the whole file is written, every CALL's offset field is patched
    // with its target's position. Patching happens after the full write
    // Returns the byte offset of main's FUNC header record.
    int64_t mainOffset=-1;
    if (yes)
    {
        for (int32_t j = 0; j<funcCount; j++)
        {
            if (funcArray[j].funcName == "main")
            {
                mainOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if (mainOffset < 0)
            cerr << "ERROR" << endl;
    }
    fclose(out);
    return mainOffset;
 
}
// PASS 0x2: EXECUTION (tokenization happens here)
enum TokenType
{
    KEYWORD,
    IDENTIFIER,
    PARAM
};
struct Token
{
    TokenType type;
    string text;
};
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    // first word is always a instruction keyword
    // instruction set = [func, func_end, call, set, add, sub, mul and div]
    // next word is identifier like name of a function, variable name
    // after identifier all are the params/arg, space separated
    int32_t count = 0;
    int i = 0;
    int len = line.length();

    while (i < len && count < maxTokens)
    {
        while (i < len && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) {
            i++;
        }

        if (i >= len) break; 
                string word = "";
        while (i < len && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') {
            word += line[i];
            i++;
        }
                tokens[count].text = word;

        if (count == 0) {
            tokens[count].type = KEYWORD;
        }
        else if (count == 1) {
            tokens[count].type = IDENTIFIER;
        }
        else {
            tokens[count].type = PARAM;
        }

        count++;
    }

    return count;
}
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    // build the snapshot based on the callStack given
    Snapshot* ss = new Snapshot;
    ss->stackDepth = callStack.snapshot_into(ss->callStack, MAX_STACK_DEPTH);
    return ss;
}
/////////////////////////////////////////
///helper functions for this
/////////////////////////////////////////
//find variable
Variable* findVar(Frame& fr, const string& name)
{
    for (int32_t i = 0; i < fr.argc; i++)
    {
        if (fr.argv[i].name == name)
            return &fr.argv[i];
    }
    for (int32_t i = 0; i < fr.localCount; i++)
    {
        if (fr.locals[i].name == name)
            return &fr.locals[i];
    }
    return NULL;
}
bool execSet(Frame& cur, Token tokens[], int32_t n, const string& text)
{
    if (n < 3)
    {
        cerr << "erroir " << text << endl;
        return false;
    }
    int32_t val = value_of_Operand(cur, tokens[2].text);
    Variable* v=Create(cur, tokens[1].text);
    if (!v)
    {
        cerr << "RUNTIME ERROR: too many variables in " << cur.func_name << endl;
        return false;
    }
    v->value = val;
    return true;
}
int32_t value_of_Operand(Frame& fr, const string& operand)
{
    if (isNumber(operand))
        return toInt(operand);
    Variable* v = findVar(fr, operand);
    if (v != NULL)
        return v->value;
    return 0;
}
Frame makeFrame(const string& name)
{
    Frame f;
    f.func_name = name;
    f.argc = 0;
    f.returnLine = -1;
    f.localCount = 0;
    return f;
}
Variable* Create(Frame& fr, const string& name)
{
    Variable* v = findVar(fr, name);
    if (v != NULL)
        return v;
    if (fr.localCount >= MAX_VARS_PER_FRAME)
        return NULL;

    fr.locals[fr.localCount].name = name;
    fr.locals[fr.localCount].value = 0;
    fr.localCount++;
    return &fr.locals[fr.localCount - 1];
}
bool math(Frame& cur, Token tokens[], int32_t n, const string& k, const string& text)
{
    if (n < 3)
    {
        cerr << " ERROR: '" << k<< "' needs 2 operands: " << text << endl;
        return false;
    }
    Variable* dst = Create(cur, tokens[1].text);
    if (!dst)
    {
        cerr << "error " << cur.func_name << endl;
        return false;
    }

    int32_t rhs = value_of_Operand(cur, tokens[2].text);

    if (k== "add")
        dst->value = dst->value + rhs;
    else if (k == "sub")
        dst->value = dst->value - rhs;
    else if (k == "mul")
        dst->value = dst->value * rhs;
    else if (k== "div")
    {
        if (rhs == 0)
        {
            cerr << "RUNTIME ERROR: division by zero: " << text << endl;
            return false;
        }
        dst->value = dst->value / rhs;
    }
    return true;
}
void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    FILE* f = fopen(resolveBinPath, "rb");
    if (!f)
    {
        cerr << "ERROR: cannot open " << resolveBinPath << endl;
        return;
    }
    // initialize the call stack
    // make the main frame
    // push main frame on the call stack

    // implementation:
    // execute line by line, and according to the keyword perform action
   

    // callerArgNames[d][i] = jis variable ka naam CALLER ne i-th argument me diya tha,
    // jo depth d par baithe frame ke liye hai. Return par isi se wapas value likhte hain
    // ("add b a" ka result main ke k me dikhna chahiye).

}

// PASS 0x3: SERIALIZE TIMELINE
void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    FILE* f = fopen(tdbgPath, "wb+");
    if (!f)
    {
        cerr << "ERROR" << tdbgPath << endl;
        return;
    }
    // placeholder for header
    TTDBHeader ttd;
    const char magicStr[] = "TTDB";
    for (int i = 0; i < 4; i++) {
        ttd.magic[i] = magicStr[i];
    }
    ttd.version = 1;
    ttd.stepCount = 0;
    ttd.indexOffset = 0;
    writeHeader(f, ttd);
    // index array of the size of stepcount from the timeline
    
    int32_t steps = timeline.getStepCount();
    int32_t indexSize;
    if (steps > 0) {
        indexSize = steps;
    }
    else {
        indexSize = 1;
    }
        int64_t* index = new int64_t[indexSize];
    // placing each snapshot in the file while maintaining the index(starting point of each nth snapshot)
        int32_t i = 0;
        for (TimelineNode* node = timeline.begin(); node != NULL; node = node->next)
        {
            int64_t currentPos = (int64_t)ftell(f);
            index[i] = currentPos;
                        
            i = i + 1;
                        if (node->data != NULL)
            {
                wrt_Snapshot(f, *node->data);
            }
        }
    // after timeline add the index array i the file
        int64_t indexOffset = (int64_t)ftell(f);
        if (steps > 0)
            fwrite(index, sizeof(int64_t), steps, f);
    // update the header
        ttd.stepCount = steps;
        ttd.indexOffset = indexOffset;
        fseek(f, 0, SEEK_SET);
        writeHeader(f, ttd);

        fclose(f);
        delete[] index;
}
// main section
int32_t main()
{

    if (!validateProgram("source.bin"))
    {
        // send an error response instead of a .tdbg file
        return 1;
    }

    int64_t mainOffset = resolveProgram("source.bin", "resolve.bin");

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    return 0;
}