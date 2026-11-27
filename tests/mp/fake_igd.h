// Galaxy 3.0 multiplayer (agent netx): a fake UPnP internet gateway on 127.0.0.1 for host tests (upnp_test.cpp, direct_test.cpp): an
// SSDP responder and an HTTP/SOAP server on one thread, with router-shaped descriptions (miniupnpd, Fritz!Box) and switchable faults.
#pragma once

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

static uint64_t NowMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec) / 1000000;
}
static void SleepMs(int ms) { usleep(ms * 1000); }

// ---- sample descriptions -------------------------------------------------------------------------------------------------------------
static std::string MiniupnpdDesc() {
    return "<?xml version=\"1.0\"?>\r\n"
           "<root xmlns=\"urn:schemas-upnp-org:device-1-0\" configId=\"1337\"><specVersion><major>1</major><minor>1</minor></specVersion>"
           "<device><deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType><friendlyName>OpenWRT router</friendlyName>"
           "<manufacturer>OpenWRT</manufacturer><modelName>OpenWRT router</modelName><UDN>uuid:12345678-0000-0000-0000-000000000000</UDN>"
           "<serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>"
           "<serviceId>urn:upnp-org:serviceId:L3Forwarding1</serviceId><SCPDURL>/L3F.xml</SCPDURL><controlURL>/ctl/L3F</controlURL>"
           "<eventSubURL>/evt/L3F</eventSubURL></service></serviceList>"
           "<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>"
           "<serviceList><service><serviceType>urn:schemas-upnp-org:service:WANCommonInterfaceConfig:1</serviceType>"
           "<serviceId>urn:upnp-org:serviceId:WANCommonIFC1</serviceId><SCPDURL>/WANCfg.xml</SCPDURL><controlURL>/ctl/CmnIfCfg</controlURL>"
           "<eventSubURL>/evt/CmnIfCfg</eventSubURL></service></serviceList>"
           "<deviceList><device><deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>"
           "<serviceList><service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>"
           "<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId><SCPDURL>/WANIPCn.xml</SCPDURL><controlURL>/ctl/IPConn</controlURL>"
           "<eventSubURL>/evt/IPConn</eventSubURL></service></serviceList></device></deviceList></device></deviceList>"
           "<presentationURL>http://192.168.1.1/</presentationURL></device></root>\r\n";
}
// Fritz!Box shape: both services, the IP one first (idle on a DSL line: no address), relative URLs, pretty-printed, a URLBase-less
// UPnP 1.0 description with the namespace on the root.
static std::string FritzDesc() {
    return "<?xml version=\"1.0\"?>\n<root xmlns=\"urn:schemas-upnp-org:device-1-0\">\n<specVersion>\n<major>1</major>\n<minor>0</minor>\n</specVersion>\n"
           "<device>\n<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>\n<friendlyName>FRITZ!Box 7590</friendlyName>\n"
           "<manufacturer>AVM Berlin</manufacturer>\n<modelName>FRITZ!Box 7590</modelName>\n<UDN>uuid:75802409-bccb-40e7-8e6c-AABBCCDDEEFF</UDN>\n"
           "<iconList>\n<icon>\n<mimetype>image/gif</mimetype>\n<width>118</width>\n<height>119</height>\n<depth>8</depth>\n<url>/ligd.gif</url>\n</icon>\n</iconList>\n"
           "<serviceList>\n<service>\n<serviceType>urn:schemas-any-com:service:Any:1</serviceType>\n<serviceId>urn:any-com:serviceId:any1</serviceId>\n"
           "<controlURL>/igdupnp/control/any</controlURL>\n<eventSubURL>/igdupnp/control/any</eventSubURL>\n<SCPDURL>/any.xml</SCPDURL>\n</service>\n</serviceList>\n"
           "<deviceList>\n<device>\n<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>\n"
           "<deviceList>\n<device>\n<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>\n<serviceList>\n"
           "<service>\n<serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType>\n<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>\n"
           "<controlURL>/igdupnp/control/WANIPConn1</controlURL>\n<eventSubURL>/igdupnp/control/WANIPConn1</eventSubURL>\n<SCPDURL>/igdconnSCPD.xml</SCPDURL>\n</service>\n"
           "<service>\n<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>\n<serviceId>urn:upnp-org:serviceId:WANPPPConn1</serviceId>\n"
           "<controlURL>/igdupnp/control/WANPPPConn1</controlURL>\n<eventSubURL>/igdupnp/control/WANPPPConn1</eventSubURL>\n<SCPDURL>/igdpppSCPD.xml</SCPDURL>\n</service>\n"
           "</serviceList>\n</device>\n</deviceList>\n</device>\n</deviceList>\n<presentationURL>http://fritz.box</presentationURL>\n</device>\n</root>\n";
}

// ---- the fake gateway ----------------------------------------------------------------------------------------------------------------
struct FakeIgd {
    // behaviour (set before Start)
    std::string desc = MiniupnpdDesc();
    enum DescMode { NORMAL, CHUNKED, SLOW, TRUNCATED, GARBAGE, NO_HEADER_END } descMode = NORMAL;
    bool answerSsdp = true;
    std::string location;          // "" = http://127.0.0.1:<tcp>/rootDesc.xml
    std::string st = "urn:schemas-upnp-org:device:InternetGatewayDevice:1";
    std::string extIp = "203.0.113.7";
    std::string emptyIpFor;        // control path whose GetExternalIPAddress answers an empty address (Fritz: the idle service)
    bool onlyPermanent = false;    // 725 for a lease != 0
    bool slowSoap = false;
    bool overwrite = false;        // corefix 8: a router that overwrites another device's mapping on AddPortMapping (instead of 718)
    bool rogue = false;            // corefix 8: another device on the LAN (127.0.0.2) that also answers the M-SEARCH, before the router
    std::atomic<int> rogueHits{0}; // connections the rogue's description server got
    int rogueTcpPort = 0, rufd = -1, rlfd = -1;
    // the router's mapping table: external port -> client "ip:port"
    std::mutex mu;
    std::map<int, std::string> table;
    std::vector<std::string> actions;   // "AddPortMapping 41100 lease=3600", ...
    int ssdpPort = 0, tcpPort = 0;
    std::atomic<bool> stop{false};
    pthread_t th{};
    int ufd = -1, lfd = -1;

    bool Start() {
        ufd = socket(AF_INET, SOCK_DGRAM, 0);
        lfd = socket(AF_INET, SOCK_STREAM, 0);
        int one = 1;
        setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(ufd, (sockaddr*)&a, sizeof a) || bind(lfd, (sockaddr*)&a, sizeof a) || listen(lfd, 8)) return false;
        if (rogue) {
            sockaddr_in r{};
            r.sin_family = AF_INET;
            r.sin_addr.s_addr = htonl(0x7F000002u);
            rufd = socket(AF_INET, SOCK_DGRAM, 0);
            rlfd = socket(AF_INET, SOCK_STREAM, 0);
            if (bind(rufd, (sockaddr*)&r, sizeof r) || bind(rlfd, (sockaddr*)&r, sizeof r) || listen(rlfd, 8)) return false;
            socklen_t rl = sizeof r;
            getsockname(rlfd, (sockaddr*)&r, &rl);
            rogueTcpPort = ntohs(r.sin_port);
        }
        socklen_t l = sizeof a;
        getsockname(ufd, (sockaddr*)&a, &l);
        ssdpPort = ntohs(a.sin_port);
        l = sizeof a;
        getsockname(lfd, (sockaddr*)&a, &l);
        tcpPort = ntohs(a.sin_port);
        return pthread_create(&th, nullptr, [](void* p) -> void* { static_cast<FakeIgd*>(p)->Run(); return nullptr; }, this) == 0;
    }
    void Stop() {
        stop = true;
        pthread_join(th, nullptr);
        close(ufd);
        close(lfd);
        if (rufd >= 0) close(rufd);
        if (rlfd >= 0) close(rlfd);
    }
    std::string Loc() const { return location.empty() ? "http://127.0.0.1:" + std::to_string(tcpPort) + "/rootDesc.xml" : location; }
    void Run() {
        while (!stop) {
            pollfd p[3] = {{ufd, POLLIN, 0}, {lfd, POLLIN, 0}, {rlfd, POLLIN, 0}};
            if (poll(p, 3, 20) <= 0) continue;
            if (rlfd >= 0 && (p[2].revents & POLLIN)) {
                const int rc = accept(rlfd, nullptr, nullptr);
                if (rc >= 0) { ++rogueHits; close(rc); }
            }
            if (p[0].revents & POLLIN) {
                char b[1500];
                sockaddr_in from{};
                socklen_t fl = sizeof from;
                const ssize_t n = recvfrom(ufd, b, sizeof b - 1, 0, (sockaddr*)&from, &fl);
                if (n > 0 && answerSsdp) {
                    b[n] = 0;
                    if (rogue && strstr(b, "M-SEARCH") && strstr(b, "ssdp:discover")) {   // (answers first, from its own address)
                        std::string rr = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: " + st + "\r\nUSN: uuid:6666::" + st +
                                         "\r\nEXT:\r\nSERVER: Linux/5.4 UPnP/1.1 Rogue/1.0\r\nLocation: http://127.0.0.2:" + std::to_string(rogueTcpPort) +
                                         "/rootDesc.xml\r\n\r\n";
                        sendto(rufd, rr.data(), rr.size(), 0, (sockaddr*)&from, fl);
                    }
                    if (strstr(b, "M-SEARCH") && strstr(b, "ssdp:discover")) {
                        std::string r = "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: " + st + "\r\nUSN: uuid:1234::" + st +
                                        "\r\nEXT:\r\nSERVER: Linux/5.4 UPnP/1.1 MiniUPnPd/2.2\r\nLocation: " + Loc() + "\r\n\r\n";
                        sendto(ufd, r.data(), r.size(), 0, (sockaddr*)&from, fl);
                    }
                }
            }
            if (p[1].revents & POLLIN) {
                const int c = accept(lfd, nullptr, nullptr);
                if (c >= 0) { Serve(c); close(c); }
            }
        }
    }
    bool ReadRequest(int c, std::string* head, std::string* body) {
        std::string in;
        const uint64_t until = NowMs() + 2000;
        while (NowMs() < until && !stop) {
            pollfd p{c, POLLIN, 0};
            if (poll(&p, 1, 20) <= 0) continue;
            char b[4096];
            const ssize_t n = recv(c, b, sizeof b, 0);
            if (n <= 0) return false;
            in.append(b, size_t(n));
            const size_t he = in.find("\r\n\r\n");
            if (he == std::string::npos) continue;
            *head = in.substr(0, he);
            size_t cl = 0;
            const char* h = strcasestr(head->c_str(), "Content-Length:");
            if (h) cl = strtoul(h + 15, nullptr, 10);
            if (in.size() - he - 4 >= cl) { *body = in.substr(he + 4, cl); return true; }
        }
        return false;
    }
    void SendAll(int c, const std::string& s) {
        size_t o = 0;
        while (o < s.size()) {
            const ssize_t n = send(c, s.data() + o, s.size() - o, MSG_NOSIGNAL);
            if (n <= 0) return;
            o += size_t(n);
        }
    }
    void Wait(int ms) { for (int i = 0; i < ms / 10 && !stop; ++i) SleepMs(10); }
    static std::string Arg(const std::string& body, const char* name) {
        const std::string o = std::string("<") + name + ">", e = std::string("</") + name + ">";
        const size_t a = body.find(o);
        if (a == std::string::npos) return "";
        const size_t b = body.find(e, a);
        return b == std::string::npos ? "" : body.substr(a + o.size(), b - a - o.size());
    }
    static std::string Soap(const std::string& action, const std::string& inner) {
        return "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
               "<s:Body><u:" + action + "Response xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\">" + inner + "</u:" + action +
               "Response></s:Body></s:Envelope>\r\n";
    }
    static std::string Fault(int code, const char* text) {
        return "<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
               "<s:Body><s:Fault><faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\">"
               "<errorCode>" + std::to_string(code) + "</errorCode><errorDescription>" + text + "</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>\r\n";
    }
    static std::string Http(int status, const std::string& body) {
        return "HTTP/1.1 " + std::to_string(status) + (status == 200 ? " OK" : " Internal Server Error") +
               "\r\nContent-Type: text/xml; charset=\"utf-8\"\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) +
               "\r\nServer: Linux UPnP/1.0 Fake\r\n\r\n" + body;
    }
    void Serve(int c) {
        std::string head, body;
        if (!ReadRequest(c, &head, &body)) return;
        if (head.compare(0, 4, "GET ") == 0) {
            switch (descMode) {
            case NORMAL: SendAll(c, Http(200, desc)); return;
            case CHUNKED: {
                std::string r = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nContent-Type: text/xml\r\n\r\n";
                for (size_t o = 0; o < desc.size(); o += 700) {
                    const std::string part = desc.substr(o, 700);
                    char h[16];
                    snprintf(h, sizeof h, "%zx;ext=1\r\n", part.size());
                    r += h + part + "\r\n";
                }
                r += "0\r\n\r\n";
                SendAll(c, r);
                return;
            }
            case SLOW: Wait(5000); SendAll(c, Http(200, desc)); return;
            case TRUNCATED: {
                std::string r = Http(200, desc);
                const size_t cut = r.find("\r\n\r\n") + 4 + 120;
                SendAll(c, r.substr(0, cut));
                return;
            }
            case GARBAGE: {
                static const char kJunk[] = "\x01\x02garbage\0\xff\xfe not http at all\r\n\r\n";
                SendAll(c, std::string(kJunk, sizeof kJunk - 1));
                return;
            }
            case NO_HEADER_END: SendAll(c, "HTTP/1.1 200 OK\r\n" + std::string(20000, 'X')); return;
            }
            return;
        }
        // SOAP
        const char* sa = strcasestr(head.c_str(), "SOAPAction:");
        std::string action = sa ? std::string(sa + 11, strcspn(sa + 11, "\r\n")) : "";
        const size_t hash = action.find('#');
        action = hash == std::string::npos ? "" : action.substr(hash + 1);
        while (!action.empty() && (action.back() == '"' || action.back() == ' ')) action.pop_back();
        const std::string path = head.substr(5, head.find(' ', 5) - 5);
        if (slowSoap) Wait(5000);
        std::lock_guard<std::mutex> g(mu);
        if (action == "GetExternalIPAddress") {
            actions.push_back("GetExternalIPAddress " + path);
            SendAll(c, Http(200, Soap(action, "<NewExternalIPAddress>" + (path == emptyIpFor ? std::string() : extIp) + "</NewExternalIPAddress>")));
            return;
        }
        const int ext = atoi(Arg(body, "NewExternalPort").c_str());
        const std::string client = Arg(body, "NewInternalClient") + ":" + Arg(body, "NewInternalPort");
        if (action == "AddPortMapping") {
            const int lease = atoi(Arg(body, "NewLeaseDuration").c_str());
            actions.push_back("AddPortMapping " + std::to_string(ext) + " lease=" + std::to_string(lease) + " " + client + " " +
                              Arg(body, "NewPortMappingDescription"));
            if (onlyPermanent && lease != 0) { SendAll(c, Http(500, Fault(725, "OnlyPermanentLeasesSupported"))); return; }
            auto it = table.find(ext);
            if (it != table.end() && it->second != client && !overwrite) { SendAll(c, Http(500, Fault(718, "ConflictInMappingEntry"))); return; }
            table[ext] = client;
            SendAll(c, Http(200, Soap(action, "")));
            return;
        }
        if (action == "GetSpecificPortMappingEntry") {
            actions.push_back("GetSpecificPortMappingEntry " + std::to_string(ext));
            auto it = table.find(ext);
            if (it == table.end()) { SendAll(c, Http(500, Fault(714, "NoSuchEntryInArray"))); return; }
            const size_t colon = it->second.find(':');
            SendAll(c, Http(200, Soap(action, "<NewInternalPort>" + it->second.substr(colon + 1) + "</NewInternalPort><NewInternalClient>" +
                                                  it->second.substr(0, colon) + "</NewInternalClient><NewEnabled>1</NewEnabled>"
                                                  "<NewPortMappingDescription>other</NewPortMappingDescription><NewLeaseDuration>1200</NewLeaseDuration>")));
            return;
        }
        if (action == "DeletePortMapping") {
            actions.push_back("DeletePortMapping " + std::to_string(ext));
            table.erase(ext);
            SendAll(c, Http(200, Soap(action, "")));
            return;
        }
        SendAll(c, Http(500, Fault(401, "Invalid Action")));
    }
    int Count(const char* prefix) {
        std::lock_guard<std::mutex> g(mu);
        int n = 0;
        for (auto& a : actions) if (a.compare(0, strlen(prefix), prefix) == 0) ++n;
        return n;
    }
};

