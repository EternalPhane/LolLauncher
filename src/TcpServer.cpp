#pragma warning(disable : 4996)
#include "TcpServer.hpp"
#include "Error.hpp"
#include "Util.hpp"

using raw_bytes = Protocol::raw_bytes;
using eptr_list = TcpServer::eptr_list;

const std::chrono::milliseconds TcpServer::SLEEP_TIME(200);

TcpServer::TcpServer(const std::string &port, Protocol &&protocol) : port(port), protocol(protocol) {}

TcpServer::~TcpServer() { stop(); }

void TcpServer::cleanup(SOCKET socket)
{
    if (socket != INVALID_SOCKET)
        closesocket(socket);
    WSACleanup();
}

std::string TcpServer::getErrMsg(const char *tag, int err_no)
{
    std::string err_msg("TcpServer ");
    return err_msg + tag + " failed: " + Util::getErrMsg(err_no);
}

SOCKET TcpServer::bind()
{
    WSADATA wsa;
    int err_no;
    if (err_no = WSAStartup(MAKEWORD(2, 2), &wsa))
        throw Error(getErrMsg("WSAStartup", err_no));
    addrinfo hints, *tmp;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;
    if (getaddrinfo(NULL, port.c_str(), &hints, &tmp))
    {
        cleanup();
        throw Error(getErrMsg("getaddrinfo", WSAGetLastError()));
    }
    std::unique_ptr<addrinfo, void(__stdcall *)(addrinfo *)> info(tmp, &freeaddrinfo);
    SOCKET listen_socket = INVALID_SOCKET;
    listen_socket = socket(info->ai_family, info->ai_socktype, info->ai_protocol);
    if (listen_socket == INVALID_SOCKET)
    {
        cleanup();
        throw Error(getErrMsg("socket", WSAGetLastError()));
    }
    if (::bind(listen_socket, info->ai_addr, (int)info->ai_addrlen) == SOCKET_ERROR)
    {
        cleanup(listen_socket);
        throw Error(getErrMsg("bind", WSAGetLastError()));
    }
    return listen_socket;
}

void TcpServer::communicate(SOCKET socket)
{
    if (listen(socket, SOMAXCONN) == SOCKET_ERROR)
    {
        cleanup(socket);
        throw Error(getErrMsg("listen", WSAGetLastError()));
    }
    SOCKET client_socket = INVALID_SOCKET;
    client_socket = accept(socket, NULL, NULL);
    if (client_socket == INVALID_SOCKET)
    {
        cleanup(socket);
        throw Error(getErrMsg("accept", WSAGetLastError()));
    }
    closesocket(socket);
    unsigned long nonblock = 1;
    if (ioctlsocket(client_socket, FIONBIO, &nonblock) == SOCKET_ERROR)
    {
        cleanup(client_socket);
        throw Error(getErrMsg("ioctlsocket", WSAGetLastError()));
    }
    auto t_send = std::thread(
        [this, client_socket] {
            try
            {
                sendLoop(client_socket);
                qSend = {};
            }
            catch (...)
            {
                eSend = std::current_exception();
            }
        });
    protocol.onConnect();
    auto t_recv = std::thread(
        [this, client_socket] {
            try
            {
                recvLoop(client_socket);
            }
            catch (...)
            {
                eRecv = std::current_exception();
            }
        });
    t_recv.join();
    protocol.onClose();
    t_send.join();
    if (shutdown(client_socket, SD_SEND) == SOCKET_ERROR)
    {
        cleanup(client_socket);
        throw Error(getErrMsg("shutdown", WSAGetLastError()));
    }
    cleanup(client_socket);
}

void TcpServer::recvLoop(SOCKET socket)
{
    int result, recv_buf_len = 1024;
    raw_bytes recv_buf(recv_buf_len);
    while (activate.load())
    {
        result = recv(socket, recv_buf.data(), recv_buf_len, 0);
        if (result == 0)
            break;
        if (result == SOCKET_ERROR)
        {
            result = WSAGetLastError();
            if (result == WSAEWOULDBLOCK)
            {
                std::this_thread::sleep_for(SLEEP_TIME);
                continue;
            }
            throw Error(getErrMsg("recv", result));
        }
        recv_buf.resize(result);
        if (protocol.onRecv(recv_buf) && protocol.onHandle())
            send(std::forward<raw_bytes>(protocol.onSend()));
    }
}

void TcpServer::sendLoop(SOCKET socket)
{
    int result;
    while (true)
    {
        std::lock_guard<std::mutex> lock(mutexSend);
        if (!activate.load() && qSend.empty())
            break;
        if (qSend.empty())
        {
            std::this_thread::sleep_for(SLEEP_TIME);
            continue;
        }
        raw_bytes &send_buf = qSend.front();
        result = ::send(socket, send_buf.data(), send_buf.size(), 0);
        if (result == SOCKET_ERROR)
        {
            result = WSAGetLastError();
            if (result == WSAEWOULDBLOCK)
            {
                std::this_thread::sleep_for(SLEEP_TIME);
                continue;
            }
            throw Error(getErrMsg("send", result));
        }
        if (result == send_buf.size())
            qSend.pop();
        else
        {
            std::copy(send_buf.begin() + result, send_buf.end(), send_buf.begin());
            send_buf.resize(send_buf.size() - result);
        }
    }
}

int TcpServer::status() { return activate.load(); }

void TcpServer::start()
{
    if (activate.load() >= 0)
        throw Error("TcpServer is still running!");
    activate++;
    eComm = eRecv = eSend = nullptr;
    SOCKET listen_socket = bind();
    tComm = std::thread(
        [this, listen_socket] {
            try
            {
                communicate(listen_socket);
            }
            catch (...)
            {
                eComm = std::current_exception();
            }
        });
    activate++;
}

void TcpServer::send(raw_bytes &&data)
{
    if (activate.load() < 0)
        throw Error("TcpServer is not running!");
    std::lock_guard<std::mutex> lock(mutexSend);
    qSend.push(std::forward<raw_bytes>(data));
}

eptr_list TcpServer::stop()
{
    if (activate.load() <= 0)
        throw Error("TcpServer is not running!");
    activate--;
    if (tComm.joinable())
        tComm.join();
    eptr_list eptrs;
    if (eComm)
        eptrs.push_back(eComm);
    if (eRecv)
        eptrs.push_back(eRecv);
    if (eSend)
        eptrs.push_back(eSend);
    activate--;
    return eptrs;
}
