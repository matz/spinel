# TCP completes, but the accepted peer never sends a TLS ServerHello.
require "openssl"
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
t = Thread.new do
  c = server.accept
  # Consume the ClientHello before waiting for the client's timeout/close.
  c.readpartial(16384)
  IO.select([c], nil, nil, 2)
  c.close
end
http = Net::HTTP.new("127.0.0.1", server.addr[1], nil)
http.use_ssl = true
http.open_timeout = 0.2
http.read_timeout = 5
started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
begin
  http.start { puts "connected" }
rescue Net::OpenTimeout => e
  puts e.class
rescue OpenSSL::SSL::SSLError => e
  puts e.class
end
p Process.clock_gettime(Process::CLOCK_MONOTONIC) - started < 1.5
t.value
server.close
