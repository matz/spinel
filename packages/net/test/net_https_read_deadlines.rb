# TLS verification settings and per-read deadlines use the local test CA.
require "openssl"
require "net/http"

defaults = Net::HTTP.new("127.0.0.1", 1, nil)
p [defaults.verify_hostname, defaults.cert_store]
defaults.verify_hostname = false
p defaults.verify_hostname

LEAF_PEM = <<~PEM
-----BEGIN CERTIFICATE-----
MIIBdDCCARmgAwIBAgICFOUwCgYIKoZIzj0EAwIwHDEaMBgGA1UEAwwRSGFuIFJp
dmVyIFRlc3QgQ0EwIBcNMjYxMDE3MTMxNTUzWhgPMjEyNjA5MjMxMzE1NTNaMCMx
ITAfBgNVBAMMGHR1cm4uamF5YW5nLWRvbmcuZXhhbXBsZTBZMBMGByqGSM49AgEG
CCqGSM49AwEHA0IABLd+fb/6Dh7v0j8wTq9i9pMngYdBanyRG+Gx0hNVWGPdXG6v
lmzEDNShpSzBykTn2fdUaBUldnyOAWzdNprHF2CjQjBAMB0GA1UdDgQWBBRSzkcf
82FjWxf5FScoX08DsF8reTAfBgNVHSMEGDAWgBSjujTEcYqXXjx+n6wC/MwOL5VK
0TAKBggqhkjOPQQDAgNJADBGAiEA9iM/A6HVmXKQL7XDGgjN7f2wiWkZ6calAR65
i16rYbcCIQDjR0XO8BgjrG6mI7C5NevMDdYCuW4qRPD35tzRvssOYw==
-----END CERTIFICATE-----
PEM
KEY_PEM = <<~PEM
-----BEGIN EC PRIVATE KEY-----
MHcCAQEEIPIRlQfBz7+HavOzW8H69dntBcUZ0MicF3/FxGctM0W1oAoGCCqGSM49
AwEHoUQDQgAEt359v/oOHu/SPzBOr2L2kyeBh0FqfJEb4bHSE1VYY91cbq+WbMQM
1KGlLMHKROfZ91RoFSV2fI4BbN02mscXYA==
-----END EC PRIVATE KEY-----
PEM
CA_PEM = <<~PEM
-----BEGIN CERTIFICATE-----
MIIBjzCCATWgAwIBAgIUI9ic9bX/0AmUyUWTCXiglCPfR0UwCgYIKoZIzj0EAwIw
HDEaMBgGA1UEAwwRSGFuIFJpdmVyIFRlc3QgQ0EwIBcNMjYxMDE3MTMxNTUzWhgP
MjEyNjA5MjMxMzE1NTNaMBwxGjAYBgNVBAMMEUhhbiBSaXZlciBUZXN0IENBMFkw
EwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEE4L0e4gJjpuc16X4tfwznrmlc4D+z3vQ
fGOiB2cJwwEhNaWDNk95IGW5n5WF64ObqEYcFpokFyWur1o4maXi1KNTMFEwHQYD
VR0OBBYEFKO6NMRxipdePH6frAL8zA4vlUrRMB8GA1UdIwQYMBaAFKO6NMRxipde
PH6frAL8zA4vlUrRMA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIg
XYulJlIdEJaj58GW085QwX6x3q2emzlns3++66sg1bsCIQCx71OOJjZW1fB6A8vh
c9tqElaq1Z6KVZ3MlZ1IgrVQpQ==
-----END CERTIFICATE-----
PEM


CONTEXT = OpenSSL::SSL::SSLContext.new
CONTEXT.add_certificate(OpenSSL::X509::Certificate.new(LEAF_PEM), OpenSSL::PKey.read(KEY_PEM))
STORE = OpenSSL::X509::Store.new
STORE.add_cert(OpenSSL::X509::Certificate.new(CA_PEM))

# Delay connection preparation as well as the peer's handshake. Each phase
# fits open_timeout, while one deadline starting before TCP would expire.
class DelayedHTTP < Net::HTTP
  def conn_address
    sleep 1
    super
  end
end

# A trusted name, a mismatched name with checking disabled, and failures with
# checking enabled or without the test CA in the trust store.
def verified_visit(host, hostname_check, trusted, delayed = false)
  server = TCPServer.new("127.0.0.1", 0)
  t = Thread.new do
    c = server.accept
    ssl = OpenSSL::SSL::SSLSocket.new(c, CONTEXT)
    begin
      sleep 1 if delayed
      ssl.accept
      while (line = ssl.gets)
        break if line.strip.empty?
      end
      ssl.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
    rescue OpenSSL::SSL::SSLError, Errno::ECONNRESET, Errno::EPIPE
      # A rejected certificate can terminate either side of the handshake.
    ensure
      ssl.close
      c.close unless c.closed?
    end
  end
  http = delayed ? DelayedHTTP.new(host, server.addr[1], nil) : Net::HTTP.new(host, server.addr[1], nil)
  http.ipaddr = "127.0.0.1"
  http.use_ssl = true
  http.open_timeout = delayed ? 1.5 : nil
  http.cert_store = STORE if trusted
  http.verify_hostname = hostname_check unless hostname_check.nil?
  http.max_retries = 0
  started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  begin
    p http.get("/").body
  rescue OpenSSL::SSL::SSLError => e
    puts e.class
  end
  p Process.clock_gettime(Process::CLOCK_MONOTONIC) - started > 1.5 if delayed
  t.value
  server.close
end

verified_visit("turn.jayang-dong.example", nil, true)
verified_visit("wrong.example", false, true)
verified_visit("wrong.example", true, true)
verified_visit("turn.jayang-dong.example", nil, false)
verified_visit("wrong.example", false, false)
verified_visit("turn.jayang-dong.example", nil, true, true)

# The bytes can already be in OpenSSL's record buffer when the next read
# begins. Reading nonblock before waiting also handles this without a timeout.
def tls_body(prefix, slow)
  server = TCPServer.new("127.0.0.1", 0)
  t = Thread.new do
    c = server.accept
    ssl = OpenSSL::SSL::SSLSocket.new(c, CONTEXT)
    begin
      ssl.accept
      while (line = ssl.gets)
        break if line.strip.empty?
      end
      ssl.write(prefix)
      if slow
        8.times do
          sleep 0.2
          ssl.write("x")
        end
      else
        IO.select([c], nil, nil, 2)
      end
    ensure
      ssl.close
      c.close unless c.closed?
    end
  end
  http = Net::HTTP.new("wrong.example", server.addr[1], nil)
  http.ipaddr = "127.0.0.1"
  http.use_ssl = true
  http.verify_hostname = false
  http.cert_store = STORE
  http.read_timeout = slow ? 1 : 0.2
  http.max_retries = 0
  started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  begin
    p http.get("/").body
  rescue Net::ReadTimeout => e
    puts e.class
  end
  elapsed = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started
  p slow ? elapsed > 1 : elapsed < 1.5
  t.value
  server.close
end

length = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n"
chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
tls_body(length, false)
tls_body(length + "abc", false)
tls_body(chunked, false)
tls_body(chunked + "6\r\nabc", false)
tls_body("HTTP/1.1 200 OK\r\nContent-Length: 8\r\nConnection: close\r\n\r\n", true)
