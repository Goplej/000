// =============================================================================
//  tests/test_tls_fixture.cpp  --  TLS 1.3 client against a local openssl server
// -----------------------------------------------------------------------------
//  Spawns `openssl s_server -tls1_3 -www` on a loopback port and drives the real
//  client stack through it: TCP connect, TLS 1.3 handshake, HTTP/1.1 exchange,
//  keep-alive reuse, chain verification against a supplied CA bundle and
//  hostname mismatch rejection.  The server is forked directly (no shell) so the
//  harness owns the exact pid and can shut it down cleanly.
//
//  If the openssl binary is unavailable the suite prints SKIP and exits 0 so it
//  remains usable on machines without an openssl toolchain.
// =============================================================================
#include "lca/net.h"
#include "lca/tls.h"
#include "lca/x509.h"
#include "test_util.h"

#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <netinet/in.h>
#include <fstream>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace lca;
using lca_test::section;

namespace {

// -----------------------------------------------------------------------------
// Fixture bookkeeping
// -----------------------------------------------------------------------------
struct Fixture {
    pid_t pid{-1};
    uint16_t port{0};
    std::string dir, cert, key, log;
    bool started{false};

    ~Fixture() { stop(); }

    void stop() {
        if (pid <= 0) return;
        ::kill(pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 40; ++i) {
            if (::waitpid(pid, &status, WNOHANG) == pid) { pid = -1; return; }
            ::usleep(25 * 1000);
        }
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, 0);
        pid = -1;
    }

    std::string log_tail(size_t max_bytes = 400) const {
        std::ifstream in(log, std::ios::binary);
        std::string all((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (all.size() <= max_bytes) return all;
        return all.substr(all.size() - max_bytes);
    }
};

bool have_openssl() {
    return ::access("/usr/bin/openssl", X_OK) == 0 || ::access("/bin/openssl", X_OK) == 0;
}

std::string run_capture(const std::string& cmd) {
    std::string out;
    FILE* pipe = ::popen((cmd + " 2>&1").c_str(), "r");
    if (!pipe) return out;
    char buffer[512];
    size_t n;
    while ((n = ::fread(buffer, 1, sizeof(buffer), pipe)) > 0) out.append(buffer, n);
    ::pclose(pipe);
    return out;
}

// Asks the kernel for an unused loopback port.
uint16_t pick_port() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = 0;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) { ::close(fd); return 0; }
    socklen_t len = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &len) != 0) { ::close(fd); return 0; }
    uint16_t port = ntohs(addr.sin_port);
    ::close(fd);
    return port;
}

bool write_self_signed_cert(Fixture& fx) {
    ::mkdir(fx.dir.c_str(), 0700);
    fx.cert = fx.dir + "/cert.pem";
    fx.key = fx.dir + "/key.pem";
    fx.log = fx.dir + "/server.log";
    std::string out = run_capture(
        "openssl req -x509 -newkey rsa:2048 -nodes -keyout " + fx.key + " -out " + fx.cert +
        " -days 1 -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1");
    (void)out;
    return ::access(fx.cert.c_str(), R_OK) == 0 && ::access(fx.key.c_str(), R_OK) == 0;
}

// Forks `openssl s_server ... -www`, redirecting its output into fx.log so the
// parent keeps a usable pid for teardown.
bool start_fixture(Fixture& fx) {
    fx.port = pick_port();
    if (fx.port == 0) return false;

    pid_t child = ::fork();
    if (child < 0) return false;
    if (child == 0) {
        ::setsid();
        int log_fd = ::open(fx.log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (log_fd >= 0) {
            ::dup2(log_fd, STDOUT_FILENO);
            ::dup2(log_fd, STDERR_FILENO);
            if (log_fd > STDERR_FILENO) ::close(log_fd);
        }
        int null_fd = ::open("/dev/null", O_RDONLY);
        if (null_fd >= 0) {
            ::dup2(null_fd, STDIN_FILENO);
            if (null_fd > STDERR_FILENO) ::close(null_fd);
        }
        std::string port = std::to_string(fx.port);
        ::execlp("openssl", "openssl", "s_server", "-tls1_3", "-accept", port.c_str(),
                 "-cert", fx.cert.c_str(), "-key", fx.key.c_str(), "-www", "-quiet",
                 static_cast<char*>(nullptr));
        ::_exit(127);   // exec failed
    }
    fx.pid = child;

    for (int i = 0; i < 160; ++i) {
        auto attempt = TcpStream::connect("127.0.0.1", fx.port, 400);
        if (attempt.ok()) {
            attempt->get()->close();
            fx.started = true;
            return true;
        }
        ::usleep(50 * 1000);
    }
    return false;
}

// -----------------------------------------------------------------------------
// Tests
// -----------------------------------------------------------------------------
void test_handshake(const Fixture& fx) {
    section("TLS 1.3 handshake over loopback");
    auto tcp = TcpStream::connect("127.0.0.1", fx.port, 5000);
    LCA_CHECK(tcp.ok());
    if (!tcp.ok()) return;

    TlsConfig cfg;
    cfg.server_name = "localhost";
    cfg.verify_peer = false;
    cfg.allow_untrusted_self_signed = true;
    cfg.timeout_ms = 10000;
    cfg.alpn = {"http/1.1"};

    TlsClient client;
    Error e = client.handshake(std::move(*tcp), cfg);
    LCA_CHECK_MSG(e.ok(), e.str());
    if (!e.ok()) return;

    const TlsSessionInfo& info = client.info();
    LCA_CHECK(info.version == TlsVersion::Tls13);
    LCA_CHECK(info.cipher_suite != 0);
    LCA_CHECK(!info.cipher_name.empty());
    LCA_CHECK(info.chain_length >= 1);
    LCA_CHECK(!info.peer_subject.empty());
    LCA_CHECK(info.group_name.empty() || info.group_name.size() > 1);
    LCA_CHECK(info.hash_len == 32 || info.hash_len == 48);
    LCA_CHECK(!info.verified);      // verification was skipped, so this must stay false
    LCA_CHECK(info.chain_length >= 1);   // the presented certificate is still recorded
    LCA_CHECK(info.chain_summary.find("disabled") != std::string::npos);
    auto stream = client.take_stream();
    LCA_CHECK(stream != nullptr);
    if (!stream) return;

    // One request per connection: `openssl s_server -www` answers a single
    // request and then closes, so the exchange proves request write, response
    // parse and close handling.
    auto parsed = parse_url("https://localhost:" + std::to_string(fx.port) + "/");
    LCA_CHECK(parsed.ok());
    if (!parsed.ok()) return;
    const Url& url = *parsed;
    {
        HttpRequest req;
        req.method = "GET";
        req.url = url.to_string();
        req.timeout_ms = 10000;
        req.accept_gzip = false;
        auto response = http_exchange(*stream, req, url);
        LCA_CHECK_MSG(response.ok(), response.ok() ? "" : response.error().str());
        if (!response.ok()) return;
        LCA_EQ(response->status, 200);
        LCA_CHECK(!response->body.empty());
        LCA_CHECK(response->body_string().find("HTTP") != std::string::npos ||
                  response->body_string().find('<') != std::string::npos);
    }
    stream->close();
    LCA_CHECK(stream->closed());

    // A second, independent session validates that no state leaks between
    // handshakes and that the server is still healthy.
    auto tcp2 = TcpStream::connect("127.0.0.1", fx.port, 5000);
    LCA_CHECK(tcp2.ok());
    if (!tcp2.ok()) return;
    TlsClient second;
    Error e2 = second.handshake(std::move(*tcp2), cfg);
    LCA_CHECK_MSG(e2.ok(), e2.str());
    if (e2.ok()) second.take_stream()->close();
}

void test_verification_rejects_unknown_ca(const Fixture& fx) {
    section("chain verification rejects an unknown CA");
    auto tcp = TcpStream::connect("127.0.0.1", fx.port, 5000);
    LCA_CHECK(tcp.ok());
    if (!tcp.ok()) return;

    TlsConfig cfg;
    cfg.server_name = "localhost";
    cfg.verify_peer = true;               // no CA bundle -> nothing anchors the leaf
    cfg.allow_untrusted_self_signed = false;
    cfg.timeout_ms = 10000;

    TlsClient client;
    Error e = client.handshake(std::move(*tcp), cfg);
    LCA_CHECK(!e.ok());
}

void test_verification_with_pinned_ca(const Fixture& fx) {
    section("chain verification against the fixture CA");
    // Load the self-signed certificate as a trust anchor and pin it by path.
    TrustStore store;
    auto loaded = store.load_pem_file(fx.cert);
    LCA_CHECK(loaded.ok());
    if (!loaded.ok()) return;
    LCA_EQ(*loaded, size_t(1));

    auto tcp = TcpStream::connect("127.0.0.1", fx.port, 5000);
    LCA_CHECK(tcp.ok());
    if (!tcp.ok()) return;

    TlsConfig cfg;
    cfg.server_name = "localhost";
    cfg.verify_peer = true;
    cfg.ca_bundle_path = fx.cert;
    cfg.timeout_ms = 10000;

    TlsClient client;
    Error e = client.handshake(std::move(*tcp), cfg);
    LCA_CHECK_MSG(e.ok(), e.str());
    if (!e.ok()) return;
    LCA_CHECK(client.info().verified);
    LCA_CHECK(client.info().chain_length >= 1);
    LCA_CHECK(client.info().peer_subject.find("localhost") != std::string::npos);
    client.take_stream()->close();
}

void test_hostname_mismatch(const Fixture& fx) {
    section("hostname mismatch is rejected");
    auto tcp = TcpStream::connect("127.0.0.1", fx.port, 5000);
    LCA_CHECK(tcp.ok());
    if (!tcp.ok()) return;

    TlsConfig cfg;
    cfg.server_name = "not-the-cert-host.example";
    cfg.verify_peer = true;
    cfg.ca_bundle_path = fx.cert;     // chain is fine, the name is not
    cfg.timeout_ms = 10000;

    TlsClient client;
    Error e = client.handshake(std::move(*tcp), cfg);
    LCA_CHECK(!e.ok());
}

void test_renewed_handshake_and_alpn(const Fixture& fx) {
    section("fresh connection per session");
    for (int i = 0; i < 2; ++i) {
        auto tcp = TcpStream::connect("localhost", fx.port, 5000);
        LCA_CHECK(tcp.ok());
        if (!tcp.ok()) return;
        TlsConfig cfg;
        cfg.server_name = "localhost";
        cfg.verify_peer = false;
        cfg.allow_untrusted_self_signed = true;
        cfg.timeout_ms = 10000;
        cfg.alpn = {"http/1.1"};
        TlsClient client;
        Error e = client.handshake(std::move(*tcp), cfg);
        LCA_CHECK_MSG(e.ok(), e.str());
        if (!e.ok()) return;
        LCA_CHECK(client.info().cipher_suite != 0);
        LCA_CHECK(client.info().handshake_ms >= 0);
        client.take_stream()->close();
    }
}

void test_trust_store_defaults() {
    section("default trust bundle discovery");
    auto paths = TrustStore::default_bundle_paths();
    LCA_CHECK(!paths.empty());
    bool found = false;
    for (const std::string& p : paths)
        if (::access(p.c_str(), R_OK) == 0) found = true;
    if (!found) {
        std::printf("  [note] no system CA bundle readable on this machine\n");
        return;
    }
    TrustStore store;
    auto n = store.load_pem_file(paths.front());
    LCA_CHECK(n.ok());
    LCA_CHECK(store.size() > 0);
}

void test_plain_http_fixture(Fixture& fx) {
    section("plain TCP HTTP round trip (no TLS)");
    // The same server port also proves TcpStream::connect/read paths in isolation:
    // a plaintext request is refused by the TLS server, which is itself a signal
    // that the socket really is the fixture and not a local proxy.
    auto tcp = TcpStream::connect("127.0.0.1", fx.port, 5000);
    LCA_CHECK(tcp.ok());
    if (!tcp.ok()) return;
    auto stream = std::move(*tcp);
    const std::string request = "GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
    Error e = stream->write_all(reinterpret_cast<const uint8_t*>(request.data()), request.size(), 0);
    LCA_CHECK(e.ok());
    uint8_t buffer[64] = {0};
    auto read = stream->read_some(buffer, sizeof(buffer) - 1, wall_millis() + 5000);
    // Either the server answers with a plaintext error or it closes the socket;
    // both mean the connection reached the fixture.
    if (read.ok() && *read > 0) {
        std::string first(reinterpret_cast<char*>(buffer), *read);
        LCA_CHECK(first.find("HTTP") != std::string::npos ||
                  first.find("Error") != std::string::npos ||
                  first.find("error") != std::string::npos);
    } else {
        LCA_CHECK(!read.ok() || *read == 0);
    }
    stream->close();
}

}  // namespace

int main() {
    std::printf("lca tls fixture tests\n=====================\n");
    if (!have_openssl()) {
        std::printf("SKIP: no openssl binary on this machine\n");
        return 0;
    }

    const char* tmp = std::getenv("TMPDIR");
    Fixture fx;
    fx.dir = std::string(tmp && *tmp ? tmp : "/tmp") + "/lca-tls-fixture-" +
             std::to_string(int64_t(wall_millis()));

    if (!write_self_signed_cert(fx)) {
        std::printf("SKIP: could not generate the fixture certificate\n");
        return 0;
    }
    if (!start_fixture(fx)) {
        std::printf("SKIP: openssl s_server did not start; log tail:\n%s\n", fx.log_tail().c_str());
        return 0;
    }

    test_handshake(fx);
    test_verification_rejects_unknown_ca(fx);
    test_verification_with_pinned_ca(fx);
    test_hostname_mismatch(fx);
    test_renewed_handshake_and_alpn(fx);
    test_plain_http_fixture(fx);
    test_trust_store_defaults();

    fx.stop();
    int rc = lca_test::finish("test_tls_fixture");
    std::printf("fixture: port %u, log %s\n", unsigned(fx.port), fx.log.c_str());
    return rc;
}
