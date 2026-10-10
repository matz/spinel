# A gzip body that does not inflate raises Zlib::DataError. An empty body,
# or an empty gzip stream, comes back empty.
require "net/http"

empty = ["1f8b080000000000000303000000000000000000"].pack("H*")
bad_crc = ["1f8b0800000000000003cb48cdc9c96748afca2ce002000079cf510b000000"].pack("H*")
payloads = ["not gzip", bad_crc, "", empty]
server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  payloads.each do |body|
    c = server.accept
    while (line = c.gets)
      break if line.strip.empty?
    end
    c.write("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: #{body.bytesize}\r\nConnection: close\r\n\r\n")
    c.write(body)
    c.close
  end
end

http = Net::HTTP.new("127.0.0.1", port)
payloads.length.times do
  begin
    r = http.get("/")
    p [r.body.unpack1("H*"), r["content-encoding"], r["content-length"]]
  rescue Zlib::DataError => e
    puts e.class
  end
end
t.join
server.close
