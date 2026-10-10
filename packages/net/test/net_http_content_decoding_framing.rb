# The body is decoded after its framing is undone: chunked, read to EOF,
# and Content-Length.
require "net/http"

gzip = ["1f8b0800000000000003cb48cdc9c96748afca2ce002000f79cf510b000000"].pack("H*")
deflate = ["789ccb48cdc9c96748afca2ce0020018ba03d9"].pack("H*")
server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  3.times do |i|
    c = server.accept
    while (line = c.gets)
      break if line.strip.empty?
    end
    if i == 0
      c.write("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
      [gzip[0, 7], gzip[7..-1]].each do |chunk|
        c.write("#{chunk.bytesize.to_s(16)}\r\n")
        c.write(chunk)
        c.write("\r\n")
      end
      c.write("0\r\n\r\n")
    elsif i == 1
      c.write("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nConnection: close\r\n\r\n")
      c.write(gzip)
    else
      c.write("HTTP/1.1 200 OK\r\nContent-Encoding: deflate\r\nContent-Length: #{deflate.bytesize}\r\nConnection: close\r\n\r\n")
      c.write(deflate)
    end
    c.close
  end
end

3.times do
  r = Net::HTTP.get_response(URI("http://127.0.0.1:#{port}/"))
  p [r.body.unpack1("H*"), r["content-encoding"], r["content-length"], r["transfer-encoding"]]
end
t.join
server.close
