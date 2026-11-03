# Net::HTTP.get(URI(...)) reads Content-Length without a consumer block.
require "net/http"
require "openssl"

# A cache-shaped read on an untyped receiver in another method must not
# make BufferedIO's byte count a String while compiling the HTTP client.
class CachedPage
  def read_fragment
    @cache.read("key")
  end
end
CachedPage.new

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  c = server.accept
  while (line = c.gets)
    break if line.strip.empty?
  end
  c.write("HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsample")
  c.close
end
puts Net::HTTP.get(URI("http://127.0.0.1:#{port}/"))
t.value
server.close
