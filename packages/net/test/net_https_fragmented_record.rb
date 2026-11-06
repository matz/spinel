# A proxy fragments TLS bytes, so SSL needs several socket reads per record.
# Progress within every read_timeout must succeed as on CRuby.
require "openssl"
require "net/http"

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


backend = TCPServer.new("127.0.0.1", 0)
proxy = TCPServer.new("127.0.0.1", 0)
responding = Queue.new
server = Thread.new do
  c = backend.accept
  ssl = OpenSSL::SSL::SSLSocket.new(c, CONTEXT)
  begin
    ssl.accept
    while (line = ssl.gets)
      break if line.strip.empty?
    end
    responding << true
    ssl.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
  rescue OpenSSL::SSL::SSLError, IOError, SystemCallError
  ensure
    ssl.close
    c.close unless c.closed?
  end
end
relay = Thread.new do
  client = proxy.accept
  upstream = TCPSocket.new("127.0.0.1", backend.addr[1])
  outgoing = Thread.new do
    begin
      loop { upstream.write(client.readpartial(16384)) }
    rescue EOFError, IOError, SystemCallError
    end
  end
  begin
    loop do
      data = upstream.readpartial(16384)
      if responding.empty?
        client.write(data)
      else
        offset = 0
        while offset < data.bytesize
          client.write(data.byteslice(offset, 4))
          offset += 4
          sleep 0.05
        end
      end
    end
  rescue EOFError, IOError, SystemCallError
  ensure
    client.close unless client.closed?
    upstream.close unless upstream.closed?
  end
  outgoing.join
end
http = Net::HTTP.new("turn.jayang-dong.example", proxy.addr[1], nil)
http.ipaddr = "127.0.0.1"
http.use_ssl = true
http.cert_store = STORE
http.open_timeout = 10
http.read_timeout = 0.3
http.max_retries = 0
started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
begin
  p http.get("/").body
rescue Net::ReadTimeout => e
  puts e.class
end
p Process.clock_gettime(Process::CLOCK_MONOTONIC) - started > 0.3
relay.value
server.value
proxy.close
backend.close
