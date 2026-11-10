// ======================= TIME-TRAVEL DEBUGGER - LOAD BALANCER TEMPLATE =======================

#include <iostream>
#include <string>
#include <cstdlib>
#include <thread>
#include <mutex>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctime>
#include "helper.h"
using namespace std;

const char* SERVER_PATH = "./server";
const int MAX_PENDING_CLIENTS = 8;
const int STARTUP_MAX_ATTEMPTS = 50;
const int STARTUP_POLL_INTERVAL_MS = 100;
int lbPort = 5000;
string serverHost = "127.0.0.1";
int serverPort = 5001;

template <typename T>
class Queue
{
    struct Node
    {
        T data;
        Node* next;
    };
    Node* front, * rear;
    int count;

public:
    Queue()
    {
        front = nullptr;
        rear = nullptr;
        count = 0;
    }
    ~Queue()
    {
        while (!isEmpty())
            dequeue();
    }
    void enqueue(const T& val)
    {
        Node* n = new Node;
        n->data = val;
        n->next = nullptr;
        if (rear != nullptr) {
            rear->next = n;
        }
        else {
            front = n;
            rear = n;
        }
        count++;
    }
    T dequeue()
    {
        if (front == nullptr) {
            return T();
        }
        Node* temp = front;
        T val = temp->data;
        front = front->next;
        if (front == nullptr)
            rear = nullptr;
        delete temp;
        count--;
        return val;
    }
    bool isEmpty()
    {
        return count == 0;
    }
    int size()
    {
        return count;
    }
};

struct ClientInfo
{
    int sock;
    string address;
    int port;
    time_t arrivedAt;
};

Queue<ClientInfo> pendingClients;
mutex queueMutex;
volatile sig_atomic_t flag = 1;

int connectToServer(const char* host, int port)
{
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0)
        return -1;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host, &addr.sin_addr);
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

bool startServer(int port)
{
    string cmd = string(SERVER_PATH) + " " + to_string(port) + " &";
    int r = system(cmd.c_str());
    return (r == 0);
}

int connectToServerWithWakeup()
{
    int sock = connectToServer(serverHost.c_str(), serverPort);
    if (sock >= 0)
        return sock;
    if (!startServer(serverPort))
        return -1;
    for (int i = 0; i < STARTUP_MAX_ATTEMPTS; i++) {
        usleep(STARTUP_POLL_INTERVAL_MS * 1000);
        sock = connectToServer(serverHost.c_str(), serverPort);
        if (sock >= 0)
            return sock;
    }
    return -1;
}

void relayOneClient(ClientInfo& c)
{
    int serverSock = connectToServerWithWakeup();
    if (serverSock < 0) {
        sendError(c.sock, "backend server failed to start");
        close(c.sock);
        return;
    }
    char buffer[IO_BUFFER_SIZE];
    ssize_t bytesRead;

    while ((bytesRead = recv(c.sock, buffer, sizeof(buffer), 0)) > 0) {
        if (!sendAll(serverSock, buffer, bytesRead)) break;
    }
    shutdown(serverSock, SHUT_WR);
    while ((bytesRead = recv(serverSock, buffer, sizeof(buffer), 0)) > 0) {
        if (!sendAll(c.sock, buffer, bytesRead)) break;
    }
    close(serverSock);
    close(c.sock);
}

void acceptLoop(int listenSock)
{
    while (flag) {
        sockaddr_in clientAddr{};
        socklen_t addrLen = sizeof(clientAddr);
        int clientSock = accept(listenSock, (sockaddr*)&clientAddr, &addrLen);
        if (clientSock < 0) continue;
        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, ipStr, sizeof(ipStr));
        ClientInfo info;
        info.sock = clientSock;
        info.address = string(ipStr);
        info.port = ntohs(clientAddr.sin_port);
        info.arrivedAt = time(nullptr);
        lock_guard<mutex> lock(queueMutex);
        if (pendingClients.size() >= MAX_PENDING_CLIENTS) {
            sendError(clientSock, "server busy, try again later");
            close(clientSock);
        }
        else {
            pendingClients.enqueue(info);
        }
    }
}

void dispatcherLoop()
{
    while (flag) {
        ClientInfo c;
        bool hasClient = false;
        {
            lock_guard<mutex> lock(queueMutex);
            if (!pendingClients.isEmpty()) {
                c = pendingClients.dequeue();
                hasClient = true;
            }
        }
        if (hasClient) {
            relayOneClient(c);
        }
        else {
            usleep(10000);
        }
    }
}

void handler(int sg) {
    (void)sg;
    flag = 0;
}

int main(int argc, char* argv[])
{
    signal(SIGINT, handler);

    if (argc >= 2) {
        lbPort = atoi(argv[1]);
    }
    if (argc >= 4) {
        serverHost = argv[2];
        serverPort = atoi(argv[3]);
    }

    int listenSock = createListenSocket(lbPort);
    cout << "Load Balancer Running on port " << lbPort << endl;

    thread acceptor(acceptLoop, listenSock);
    dispatcherLoop();

    if (acceptor.joinable()) {
        acceptor.join();
    }
    close(listenSock);
    return 0;
}