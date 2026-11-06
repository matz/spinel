# Many writes, buffered headers and binary bytes must survive every framing.
require "net/http"

part = "a\x00b\xff" * 2048
expected = part * 256
["length", "chunked", "close"].each do |framing|
  server = TCPServer.new("127.0.0.1", 0)
  t = Thread.new do
    c = server.accept
    while (line = c.gets)
      break if line.strip.empty?
    end
    c.write("HTTP/1.1 200 OK\r\nConnection: close\r\n")
    c.write("Content-Length: #{expected.bytesize}\r\n") if framing == "length"
    c.write("Transfer-Encoding: chunked\r\n") if framing == "chunked"
    c.write("\r\n")
    256.times do
      c.write("#{part.bytesize.to_s(16)}\r\n") if framing == "chunked"
      c.write(part)
      c.write("\r\n") if framing == "chunked"
    end
    c.write("0\r\nX-End: yes\r\n\r\n") if framing == "chunked"
    c.close
  end
  http = Net::HTTP.new("127.0.0.1", server.addr[1], nil)
  http.read_timeout = nil
  body = String.new
  http.request(Net::HTTP::Get.new("/")) do |res|
    res.read_body { |fragment| body << fragment }
  end
  p [body.bytesize, body.b == expected.b]
  t.value
  server.close
end
