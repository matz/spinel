# A gzip, x-gzip or deflate body comes back decoded, without
# Content-Encoding and with its Content-Length updated. identity and none
# are dropped; an unknown encoding stays as it came.
require "net/http"

gzip = ["1f8b0800000000000003cb48cdc9c96748afca2ce002000f79cf510b000000"].pack("H*")
deflate = ["789ccb48cdc9c96748afca2ce0020018ba03d9"].pack("H*")
payloads = [gzip, gzip, deflate, "plain", "plain", "plain", "raw"]
encodings = ["gzip", "X-GZip", "deflate", nil, "identity", "none", "br"]
server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  payloads.each_with_index do |body, i|
    c = server.accept
    while (line = c.gets)
      break if line.strip.empty?
    end
    headers = "Content-Length: #{body.bytesize}\r\nConnection: close\r\n"
    headers << "Content-Encoding: #{encodings[i]}\r\n" unless encodings[i].nil?
    c.write("HTTP/1.1 200 OK\r\n#{headers}\r\n")
    c.write(body)
    c.close
  end
end

http = Net::HTTP.new("127.0.0.1", port)
payloads.length.times do
  r = http.get("/")
  p [r.body.unpack1("H*"), r["content-encoding"], r["content-length"]]
end
t.join
server.close
