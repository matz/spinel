# The body stays encoded when the caller asked for an encoding or a range,
# or when the answer has Content-Range. HEAD, 204 and 304 have no body to
# decode and keep their headers.
require "net/http"

gzip = ["1f8b0800000000000003cb48cdc9c96748afca2ce002000f79cf510b000000"].pack("H*")
server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  9.times do |i|
    c = server.accept
    while (line = c.gets)
      break if line.strip.empty?
    end
    code = i == 6 ? "204 No Content" : (i == 7 ? "304 Not Modified" : "200 OK")
    c.write("HTTP/1.1 #{code}\r\nContent-Encoding: gzip\r\nContent-Length: #{gzip.bytesize}\r\nConnection: close\r\n")
    c.write("Content-Range: bytes 0-30/100\r\n") if i == 4
    c.write("\r\n")
    c.write(gzip) if i < 5 || i == 8
    c.close
  end
end

requests = [
  Net::HTTP::Get.new("/", "accept-encoding" => "gzip"),
  Net::HTTP::Get.new("/", "Range" => "bytes=0-30"),
  Net::HTTP::Get.new("/"),
  Net::HTTP::Get.new("/"),
  Net::HTTP::Get.new("/"),
  Net::HTTP::Head.new("/"),
  Net::HTTP::Get.new("/"),
  Net::HTTP::Get.new("/"),
  Net::HTTP::Get.new("/"),
]
requests[2]["Accept-Encoding"] = "gzip"
requests[3]["Accept-Encoding"] = nil
http = Net::HTTP.new("127.0.0.1", port)
requests.each do |req|
  r = http.request(req)
  p [r.code, r.body.nil?, r.body == gzip, r["content-encoding"], r["content-length"]]
end
t.join
server.close
