// ======================= TIME-TRAVEL DEBUGGER - SERVER =======================
//
// Pipeline (upar se neeche):
//   0. Receive  -- Phase 01 me hum maan lete hain ke source.bin already disk par hai
//   1. Pass 0X0 -- validity check (FUNC / FUNC_END matching, no nesting)
//   2. Pass 0X1 -- resolve(): har line ko resolve.bin me [offset][size][string] bana ke likho,
//                  phir CALL ke offset field ko target function ke offset se patch karo
//   3. Pass 0X2 -- execute: ek line tokenize -> execute -> snapshot -> Timeline
//   4. Pass 0X3 -- Timeline ko session.tdbg me likho (header + snapshots + dense index)
//
// Compile (Linux / WSL / Windows MinGW - same command):
//     g++ -std=c++11 -Wall -o server server.cpp
// Run:
//     ./server source.bin            (Linux / WSL)
//     server.exe source.bin          (Windows)
//     ./server source.bin --show     (extra: timeline console par print karta hai, testing ke liye)
//
// NOTE: <unistd.h> aur <sys/socket.h> hata diye hain kyunke wo sirf Linux/Mac par hote hain
// aur Windows par compile error dete. Phase 01 me socket ki zaroorat nahi (source.bin direct parho).
// Koi STL container use nahi kiya (no vector/stack/map) - sirf string, jo template me already tha.

#include <iostream>
#include <string>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
using namespace std;

// ---- Constants ----
const int32_t MAX_VARS_PER_FRAME = 16;
const int32_t MAX_STACK_DEPTH = 64;
const int32_t MAX_FUNCS = 128;
const int32_t MAX_TOKENS = MAX_VARS_PER_FRAME + 2; // keyword + identifier + upto 16 params/args
const int32_t MAX_PATCHES = MAX_FUNCS * 4;
const uint64_t MAX_SOURCE_BYTES = 15ULL * 1024 * 1024; // sanity cap (Phase 02 me receive ke waqt use hoga)
const int32_t IO_BUFFER_SIZE = 64 * 1024;              // Phase 02 (socket streaming) ke liye
const int32_t SOCKET_TIMEOUT_SEC = 5;                  // TODO Phase 02: SO_RCVTIMEO

// ======================= Small helper functions (string handling) =======================

// space ya tab ya \r ya \n hai?
bool isSpaceChar(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// string ko lowercase me convert karta hai (taake FUNC aur func dono chalein)
string toLower(const string &s)
{
    string r = s;
    for (size_t i = 0; i < r.size(); i++)
    {
        if (r[i] >= 'A' && r[i] <= 'Z')
            r[i] = (char)(r[i] - 'A' + 'a');
    }
    return r;
}

// kya string ek integer number hai? ("10", "-5", "+3")
bool isNumber(const string &s)
{
    if (s.empty())
        return false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+')
    {
        if (s.size() == 1)
            return false; // sirf "-" number nahi hota
        i = 1;
    }
    for (; i < s.size(); i++)
    {
        if (s[i] < '0' || s[i] > '9')
            return false;
    }
    return true;
}

// number-string ko int me badlo (pehle isNumber() se check kar lena)
int32_t toInt(const string &s)
{
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-')
    {
        neg = true;
        i = 1;
    }
    else if (s[0] == '+')
    {
        i = 1;
    }
    int32_t v = 0;
    for (; i < s.size(); i++)
        v = v * 10 + (s[i] - '0');
    return neg ? -v : v;
}

// line me position 'pos' se agla word nikalo. word me result aata hai,
// return value = word ke BAAD wali position. Agar word nahi mila to word="" hoga.
int32_t nextWord(const string &line, int32_t pos, string &word)
{
    int32_t n = (int32_t)line.size();
    while (pos < n && isSpaceChar(line[pos])) // pehle spaces skip
        pos++;
    int32_t start = pos;
    while (pos < n && !isSpaceChar(line[pos])) // phir word padho
        pos++;
    word = line.substr(start, pos - start);
    return pos;
}

// ======================= Custom data structures =======================

// Stack: live Call Stack ko back karta hai (linked list se bana hai, array nahi)
template <typename T>
class Stack
{
    struct Node
    {
        T data;
        Node *next;
    };
    Node *top;     // sab se upar wala node
    int32_t count; // kitne elements hain

public:
    Stack()
    { // empty stack
        top = NULL;
        count = 0;
    }
    ~Stack()
    { // saari nodes free karo (memory leak se bachne ke liye)
        while (top != NULL)
        {
            Node *tmp = top;
            top = top->next;
            delete tmp;
        }
    }
    void push(const T &val)
    {
        if (count >= MAX_STACK_DEPTH) // limit cross -> push nahi karna
            return;
        Node *n = new Node;
        n->data = val;
        n->next = top; // purana top ab iske neeche
        top = n;
        count++;
    }
    T pop()
    {
        if (top == NULL) // empty stack: default value wapas
            return T();
        Node *tmp = top;
        T val = tmp->data; // value copy kar lo, phir node delete
        top = top->next;
        delete tmp;
        count--;
        return val;
    }
    T &peek()
    {
        static T dummy; // empty stack par crash na ho
        if (top == NULL)
            return dummy;
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
    // Har frame ko top se bottom tak out[] me copy karta hai. Kitne likhe wo return karta hai.
    int32_t snapshot_into(T out[], int32_t maxLen)
    {
        int32_t i = 0;
        Node *cur = top;
        while (cur != NULL && i < maxLen)
        {
            out[i] = cur->data; // out[0] = top frame
            i++;
            cur = cur->next;
        }
        return i;
    }
};

// Timeline : Snapshots ki doubly linked list
struct Snapshot; // forward declaration
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
    Timeline()
    {
        head = NULL;
        tail = NULL;
        stepCount = 0;
    }
    ~Timeline();           // neeche define hai (Snapshot ka size pata hona chahiye delete ke liye)
    void record(Snapshot *s)
    {
        // naya node bana ke list ke END me jodo
        TimelineNode *n = new TimelineNode;
        n->data = s;
        n->next = NULL;
        n->prev = tail;
        if (tail != NULL)
            tail->next = n;
        else
            head = n; // list khali thi
        tail = n;
        stepCount++;
    }
    TimelineNode *begin()
    {
        return head;
    }
    int32_t getStepCount()
    {
        return stepCount;
    }
};

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
    int32_t returnLine; // resolve.bin ka byte offset jahan se wapas aake chalna hai
    Variable locals[MAX_VARS_PER_FRAME];
    int32_t localCount;
};
struct Snapshot
{
    Frame callStack[MAX_STACK_DEPTH];
    int32_t stackDepth;
};

// Timeline ka destructor: har snapshot aur node delete
Timeline::~Timeline()
{
    TimelineNode *cur = head;
    while (cur != NULL)
    {
        TimelineNode *nxt = cur->next;
        delete cur->data;
        delete cur;
        cur = nxt;
    }
}

struct TTDBHeader
{
    char magic[4]; // "TTDB"
    int32_t version;
    int32_t stepCount;
    int64_t indexOffset;
};
// Header ko file ke current position par likhta hai (total 4+4+4+8 = 20 bytes, koi padding nahi
// kyunke hum field-by-field likh rahe hain, poora struct ek saath nahi).
void writeHeader(FILE *f, const TTDBHeader &h)
{
    fwrite(h.magic, 1, 4, f);
    fwrite(&h.version, sizeof(int32_t), 1, f);
    fwrite(&h.stepCount, sizeof(int32_t), 1, f);
    fwrite(&h.indexOffset, sizeof(int64_t), 1, f);
}

// resolve.bin - bookkeeping
struct FuncEntry
{
    string funcName;
    int64_t byteOffsetInResolveBin; // function ka FUNC header record yahan hai
};
struct PendingPatch
{
    int64_t byteOffsetOfOffsetField; // resolve.bin me yahan wapas seek karke overwrite karna hai
    string targetFuncName;
};

// ======================= PASS 0x0: source.bin padho + VALIDITY CHECK =======================

// Agli NON-BLANK line padhta hai. Windows ke \r aur aage-peeche ke spaces hata deta hai.
// false = file khatam.
bool readSourceLine(ifstream &in, string &out)
{
    string line;
    while (getline(in, line))
    {
        int32_t s = 0, e = (int32_t)line.size();
        while (s < e && isSpaceChar(line[s])) // left trim
            s++;
        while (e > s && isSpaceChar(line[e - 1])) // right trim (\r yahin hat jata hai)
            e--;
        if (e > s) // blank nahi hai
        {
            out = line.substr(s, e - s);
            return true;
        }
    }
    return false;
}
string firstWord(const string &line)
{
    string w;
    nextWord(line, 0, w);
    return w;
}
string secondWord(const string &line)
{
    string w;
    int32_t p = nextWord(line, 0, w); // pehla word skip
    nextWord(line, p, w);             // doosra word
    return w;
}
// Nesting allowed nahi, isliye stack ki zaroorat nahi: ek simple flag "insideFunc" kaafi hai
// (stack tab chahiye hoti jab nesting allowed hoti).
bool validateProgram(const char *sourcePath)
{
    ifstream in(sourcePath, ios::binary);
    if (!in)
    {
        cerr << "ERROR: cannot open " << sourcePath << endl;
        return false;
    }
    bool insideFunc = false;
    string line;
    int32_t lineNo = 0; // non-blank line number
    while (readSourceLine(in, line))
    {
        lineNo++;
        string kw = toLower(firstWord(line));
        if (kw == "func")
        {
            if (insideFunc)
            {
                cerr << "ERROR (line " << lineNo << "): nested func declaration is not allowed" << endl;
                return false;
            }
            if (secondWord(line).empty())
            {
                cerr << "ERROR (line " << lineNo << "): func has no name" << endl;
                return false;
            }
            insideFunc = true;
        }
        else if (kw == "func_end")
        {
            if (!insideFunc)
            {
                cerr << "ERROR (line " << lineNo << "): func_end without matching func" << endl;
                return false;
            }
            insideFunc = false;
        }
    }
    if (insideFunc)
    {
        cerr << "ERROR: func without matching func_end" << endl;
        return false;
    }
    return true;
}

// ======================= PASS 0x1: RESOLVE() -> resolve.bin =======================

// Ek record likhta hai: [offset(8B)][size(4B)][string]. Return = record ki apni starting position.
int64_t writeResolveRecord(FILE *f, int64_t offsetField, const string &text)
{
    int64_t pos = (int64_t)ftell(f);
    int32_t size = (int32_t)text.size();
    fwrite(&offsetField, sizeof(int64_t), 1, f);
    fwrite(&size, sizeof(int32_t), 1, f);
    if (size > 0)
        fwrite(text.c_str(), 1, size, f);
    return pos;
}
// Ek record padhta hai aur aage badh jata hai. Return = offset field (error/EOF par -1).
// Line ka text bilkul waisa ka waisa outText me aata hai.
int64_t readResolveRecord(FILE *f, string &outText)
{
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
// Return = main ke FUNC record ka offset. Error par -1 (main nahi / undefined call / etc).
// Convention: normal line ka offset field = uski apni position. CALL line ka offset field
// shuru me apni position hota hai, phir patch ho kar TARGET function ka offset ban jata hai.
int64_t resolveProgram(const char *sourcePath, const char *resolveBinPath)
{
    FuncEntry funcArray[MAX_FUNCS];
    int32_t funcCount = 0;
    PendingPatch patches[MAX_PATCHES];
    int32_t patchCount = 0;

    ifstream in(sourcePath, ios::binary);
    if (!in)
    {
        cerr << "ERROR: cannot open " << sourcePath << endl;
        return -1;
    }
    // "wb+" = binary write + read. Windows par 'b' bohot zaroori hai warna \n ke saath chhed-chhad hoti hai.
    FILE *out = fopen(resolveBinPath, "wb+");
    if (!out)
    {
        cerr << "ERROR: cannot create " << resolveBinPath << endl;
        return -1;
    }

    bool ok = true;
    string line;
    while (ok && readSourceLine(in, line))
    {
        string kw = toLower(firstWord(line));
        // Pehle record likho (offset field = apni position), position wapas milti hai
        int64_t pos = writeResolveRecord(out, (int64_t)ftell(out), line);

        if (kw == "func")
        {
            string name = secondWord(line);
            if (funcCount >= MAX_FUNCS)
            {
                cerr << "ERROR: too many functions (max " << MAX_FUNCS << ")" << endl;
                ok = false;
                break;
            }
            for (int32_t i = 0; i < funcCount; i++) // duplicate naam check
            {
                if (funcArray[i].funcName == name)
                {
                    cerr << "ERROR: function '" << name << "' defined more than once" << endl;
                    ok = false;
                    break;
                }
            }
            if (!ok)
                break;
            funcArray[funcCount].funcName = name;
            funcArray[funcCount].byteOffsetInResolveBin = pos;
            funcCount++;
        }
        else if (kw == "call")
        {
            if (patchCount >= MAX_PATCHES)
            {
                cerr << "ERROR: too many call instructions (max " << MAX_PATCHES << ")" << endl;
                ok = false;
                break;
            }
            patches[patchCount].byteOffsetOfOffsetField = pos; // offset field record ke shuru me hi hai
            patches[patchCount].targetFuncName = secondWord(line);
            patchCount++;
        }
    }

    // Poori file likhne ke BAAD patching (kyunke call kisi aisi function ko bhi ho sakti hai jo baad me aaye)
    for (int32_t i = 0; ok && i < patchCount; i++)
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
            cerr << "ERROR: call to undefined function '" << patches[i].targetFuncName << "'" << endl;
            ok = false;
            break;
        }
        fseek(out, (long)patches[i].byteOffsetOfOffsetField, SEEK_SET); // wapas jao
        fwrite(&target, sizeof(int64_t), 1, out);                       // purana offset overwrite
    }

    // main dhoondo
    int64_t mainOffset = -1;
    if (ok)
    {
        for (int32_t j = 0; j < funcCount; j++)
        {
            if (funcArray[j].funcName == "main")
            {
                mainOffset = funcArray[j].byteOffsetInResolveBin;
                break;
            }
        }
        if (mainOffset < 0)
            cerr << "ERROR: no main function found" << endl;
    }
    fclose(out);
    return mainOffset; // -1 matlab error
}

// ======================= PASS 0x2: EXECUTION (tokenization yahan hoti hai) =======================

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
// Line ko tokens me todta hai. Return = token count.
//   token[0] = KEYWORD, token[1] = IDENTIFIER, baaki sab PARAM
int32_t tokenizeLine(const string &line, Token tokens[], int32_t maxTokens)
{
    int32_t count = 0;
    int32_t pos = 0;
    string w;
    while (count < maxTokens)
    {
        pos = nextWord(line, pos, w);
        if (w.empty()) // line khatam
            break;
        tokens[count].text = w;
        if (count == 0)
            tokens[count].type = KEYWORD;
        else if (count == 1)
            tokens[count].type = IDENTIFIER;
        else
            tokens[count].type = PARAM;
        count++;
    }
    return count;
}

// Callstack ki copy bana ke Snapshot (heap par) deta hai
Snapshot *buildSnapshot(Stack<Frame> &callStack)
{
    Snapshot *s = new Snapshot;
    s->stackDepth = callStack.snapshot_into(s->callStack, MAX_STACK_DEPTH);
    return s;
}

// ---- Variable helpers ----

// Frame me variable dhoondo (pehle argv me, phir locals me). Nahi mila to NULL.
Variable *findVar(Frame &fr, const string &name)
{
    for (int32_t i = 0; i < fr.argc; i++)
        if (fr.argv[i].name == name)
            return &fr.argv[i];
    for (int32_t i = 0; i < fr.localCount; i++)
        if (fr.locals[i].name == name)
            return &fr.locals[i];
    return NULL;
}
// Dhoondo, nahi mila to naya local (value 0) bana do. Frame full ho to NULL.
Variable *getOrCreate(Frame &fr, const string &name)
{
    Variable *v = findVar(fr, name);
    if (v != NULL)
        return v;
    if (fr.localCount >= MAX_VARS_PER_FRAME)
        return NULL;
    fr.locals[fr.localCount].name = name;
    fr.locals[fr.localCount].value = 0;
    fr.localCount++;
    return &fr.locals[fr.localCount - 1];
}
// Operand ki value: number ho to wahi number, warna variable ki value (undefined = 0)
int32_t valueOf(Frame &fr, const string &operand)
{
    if (isNumber(operand))
        return toInt(operand);
    Variable *v = findVar(fr, operand);
    return v ? v->value : 0;
}
// Naya khali frame
Frame makeFrame(const string &name)
{
    Frame f;
    f.func_name = name;
    f.argc = 0;
    f.returnLine = -1;
    f.localCount = 0;
    return f;
}

void executeProgram(const char *resolveBinPath, int64_t mainOffset, Timeline &timeline)
{
    FILE *f = fopen(resolveBinPath, "rb");
    if (!f)
    {
        cerr << "ERROR: cannot open " << resolveBinPath << endl;
        return;
    }

    // callerArgNames[d][i] = jis variable ka naam CALLER ne i-th argument me diya tha,
    // jo depth d par baithe frame ke liye hai. Return par isi se wapas value likhte hain
    // ("add b a" ka result main ke k me dikhna chahiye).
    static string callerArgNames[MAX_STACK_DEPTH][MAX_VARS_PER_FRAME];

    Stack<Frame> callStack;
    Token tokens[MAX_TOKENS];
    Token calleeTokens[MAX_TOKENS];
    string text;

    // ---- main ka frame banao ----
    fseek(f, (long)mainOffset, SEEK_SET);
    readResolveRecord(f, text);                 // main ki "func main" line
    int64_t pc = (int64_t)ftell(f);             // pc = agli instruction ka byte offset
    callStack.push(makeFrame("main"));          // main frame stack par

    bool running = true;
    while (running)
    {
        fseek(f, (long)pc, SEEK_SET);
        int64_t offField = readResolveRecord(f, text); // ek line padho
        if (offField < 0)
            break; // file khatam ho gayi (func_end miss hua) -> ruk jao
        int64_t nextPc = (int64_t)ftell(f);

        int32_t n = tokenizeLine(text, tokens, MAX_TOKENS); // tokenize
        if (n == 0)
        {
            pc = nextPc;
            continue;
        }
        string kw = toLower(tokens[0].text);
        if (kw.size() >= 2 && kw[0] == '/' && kw[1] == '/') // comment line: skip, snapshot nahi
        {
            pc = nextPc;
            continue;
        }

        Frame &cur = callStack.peek(); // abhi chal raha frame (top)

        if (kw == "set")
        {
            // set a 10  /  set a b
            if (n < 3)
            {
                cerr << "RUNTIME ERROR: 'set' needs 2 operands: " << text << endl;
                break;
            }
            int32_t val = valueOf(cur, tokens[2].text);
            Variable *v = getOrCreate(cur, tokens[1].text);
            if (!v)
            {
                cerr << "RUNTIME ERROR: too many variables in " << cur.func_name << endl;
                break;
            }
            v->value = val;
            pc = nextPc;
            timeline.record(buildSnapshot(callStack));
        }
        else if (kw == "add" || kw == "sub" || kw == "mul" || kw == "div")
        {
            // add b a  ->  b = b + a  (result PEHLE parameter me)
            if (n < 3)
            {
                cerr << "RUNTIME ERROR: '" << kw << "' needs 2 operands: " << text << endl;
                break;
            }
            Variable *dst = getOrCreate(cur, tokens[1].text);
            if (!dst)
            {
                cerr << "RUNTIME ERROR: too many variables in " << cur.func_name << endl;
                break;
            }
            int32_t rhs = valueOf(cur, tokens[2].text);
            if (kw == "add")
                dst->value = dst->value + rhs;
            else if (kw == "sub")
                dst->value = dst->value - rhs;
            else if (kw == "mul")
                dst->value = dst->value * rhs;
            else
            {
                if (rhs == 0)
                {
                    cerr << "RUNTIME ERROR: division by zero: " << text << endl;
                    break;
                }
                dst->value = dst->value / rhs;
            }
            pc = nextPc;
            timeline.record(buildSnapshot(callStack));
        }
        else if (kw == "call")
        {
            // Record ka offset field patch ho chuka hai = target function ka offset
            if (callStack.depth() >= MAX_STACK_DEPTH)
            {
                cerr << "RUNTIME ERROR: stack overflow (max depth " << MAX_STACK_DEPTH << ")" << endl;
                break;
            }
            // Callee ki "func name p1 p2 ..." line padho taake parameter ke naam mil jayein
            fseek(f, (long)offField, SEEK_SET);
            string headerText;
            readResolveRecord(f, headerText);
            int64_t calleeBodyPc = (int64_t)ftell(f); // header ke theek baad se body shuru
            int32_t hn = tokenizeLine(headerText, calleeTokens, MAX_TOKENS);
            int32_t paramCount = hn - 2;
            int32_t argCount = n - 2;
            if (paramCount != argCount)
            {
                cerr << "RUNTIME ERROR: '" << calleeTokens[1].text << "' expects " << paramCount
                     << " argument(s) but got " << argCount << endl;
                break;
            }
            Frame nf = makeFrame(calleeTokens[1].text);
            nf.argc = paramCount;
            nf.returnLine = (int32_t)nextPc; // wapas isi call ki agli line par
            int32_t newIndex = callStack.depth(); // naya frame is index par baithega
            for (int32_t i = 0; i < paramCount; i++)
            {
                nf.argv[i].name = calleeTokens[2 + i].text;              // callee ka param naam (b)
                nf.argv[i].value = valueOf(cur, tokens[2 + i].text);     // caller ki value (k ki)
                callerArgNames[newIndex][i] = tokens[2 + i].text;        // yaad rakho caller ne kya naam diya
            }
            callStack.push(nf); // NOTE: iske baad 'cur' use mat karna
            pc = calleeBodyPc;
            timeline.record(buildSnapshot(callStack));
        }
        else if (kw == "func_end")
        {
            if (callStack.depth() == 1)
            {
                // main khatam -> final state ka snapshot lo aur program band
                timeline.record(buildSnapshot(callStack));
                running = false;
            }
            else
            {
                Frame done = callStack.pop();           // callee frame hatao
                int32_t idx = callStack.depth();        // ye wohi index hai jis par done baitha tha
                Frame &caller = callStack.peek();
                for (int32_t i = 0; i < done.argc; i++) // updated values caller ko wapas (by-reference jaisa)
                {
                    const string &nm = callerArgNames[idx][i];
                    if (!isNumber(nm))
                    {
                        Variable *v = getOrCreate(caller, nm);
                        if (v)
                            v->value = done.argv[i].value;
                    }
                }
                pc = done.returnLine;
                timeline.record(buildSnapshot(callStack));
            }
        }
        else
        {
            // unknown instruction: warning do aur skip (snapshot nahi)
            cerr << "WARNING: unknown instruction skipped: " << text << endl;
            pc = nextPc;
        }
    }
    fclose(f);
}

// ======================= PASS 0x3: SERIALIZE TIMELINE =======================
// Snapshot me std::string hai, isliye poora struct ek saath fwrite NAHI kar sakte
// (string ke andar pointer hota hai). Isliye field-by-field likhte hain:
//   string   = [len(4B)][bytes]
//   Variable = string name + int32 value
//   Frame    = func_name, argc, argc x Variable, returnLine, localCount, localCount x Variable
//   Snapshot = stackDepth, phir stackDepth x Frame (top frame pehle)
// Phase 02 (client) ko bilkul isi format me padhna hoga.

void writeString(FILE *f, const string &s)
{
    int32_t len = (int32_t)s.size();
    fwrite(&len, sizeof(int32_t), 1, f);
    if (len > 0)
        fwrite(s.c_str(), 1, len, f);
}
void writeVariable(FILE *f, const Variable &v)
{
    writeString(f, v.name);
    fwrite(&v.value, sizeof(int32_t), 1, f);
}
void writeFrame(FILE *f, const Frame &fr)
{
    writeString(f, fr.func_name);
    fwrite(&fr.argc, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < fr.argc; i++)
        writeVariable(f, fr.argv[i]);
    fwrite(&fr.returnLine, sizeof(int32_t), 1, f);
    fwrite(&fr.localCount, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < fr.localCount; i++)
        writeVariable(f, fr.locals[i]);
}
void writeSnapshot(FILE *f, const Snapshot &s)
{
    fwrite(&s.stackDepth, sizeof(int32_t), 1, f);
    for (int32_t i = 0; i < s.stackDepth; i++)
        writeFrame(f, s.callStack[i]);
}

void writeTdbg(Timeline &timeline, const char *tdbgPath)
{
    FILE *f = fopen(tdbgPath, "wb+");
    if (!f)
    {
        cerr << "ERROR: cannot create " << tdbgPath << endl;
        return;
    }

    // 1. Header ki jagah abhi khali (zero) header likh do - baad me sahi values se overwrite karenge
    TTDBHeader h;
    memcpy(h.magic, "TTDB", 4);
    h.version = 1;
    h.stepCount = 0;
    h.indexOffset = 0;
    writeHeader(f, h);

    // 2. Index array: index[i] = i-th snapshot ki file me starting byte position
    int32_t steps = timeline.getStepCount();
    int64_t *index = new int64_t[steps > 0 ? steps : 1];

    // 3. Har snapshot likho, likhne se PEHLE uski position index me note karo
    int32_t i = 0;
    for (TimelineNode *node = timeline.begin(); node != NULL; node = node->next)
    {
        index[i++] = (int64_t)ftell(f);
        writeSnapshot(f, *node->data);
    }

    // 4. Saare snapshots ke baad index array likho
    int64_t indexOffset = (int64_t)ftell(f);
    if (steps > 0)
        fwrite(index, sizeof(int64_t), steps, f);

    // 5. Wapas shuru me jao aur asli header likho
    h.stepCount = steps;
    h.indexOffset = indexOffset;
    fseek(f, 0, SEEK_SET);
    writeHeader(f, h);

    fclose(f);
    delete[] index;
}

// ---- Extra (testing ke liye): timeline console par dikhao ----
void printTimeline(Timeline &timeline)
{
    int32_t step = 0;
    for (TimelineNode *node = timeline.begin(); node != NULL; node = node->next)
    {
        Snapshot *s = node->data;
        cout << "--- Step " << step++ << " (stack depth " << s->stackDepth << ") ---" << endl;
        for (int32_t d = 0; d < s->stackDepth; d++)
        {
            Frame &fr = s->callStack[d];
            cout << "  " << fr.func_name << "(";
            for (int32_t i = 0; i < fr.argc; i++)
                cout << (i ? ", " : "") << fr.argv[i].name << "=" << fr.argv[i].value;
            cout << ")  locals: ";
            for (int32_t i = 0; i < fr.localCount; i++)
                cout << fr.locals[i].name << "=" << fr.locals[i].value << " ";
            cout << endl;
        }
    }
}

// ======================= main =======================
int main(int argc, char *argv[])
{
    const char *sourcePath = (argc >= 2) ? argv[1] : "source.bin";
    bool show = (argc >= 3 && string(argv[2]) == "--show");

    if (!validateProgram(sourcePath))
    {
        // Phase 02 me yahan client ko error response bheja jayega; abhi stderr par print ho chuka hai
        return 1;
    }

    int64_t mainOffset = resolveProgram(sourcePath, "resolve.bin");
    if (mainOffset < 0) // main nahi mila ya undefined call -> execute nahi karna
        return 1;

    Timeline timeline;
    executeProgram("resolve.bin", mainOffset, timeline);

    writeTdbg(timeline, "session.tdbg");

    cout << "Done! " << timeline.getStepCount() << " steps written to session.tdbg" << endl;
    if (show)
        printTimeline(timeline);
    return 0;
}
