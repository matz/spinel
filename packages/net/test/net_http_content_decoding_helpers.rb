# Net::HTTP#get, #head and #post hand their headers to the request, so a
# Range in them, even nil, leaves the body encoded and sends no
# Accept-Encoding.
require "net/http"

gzip = ["1f8b0800000000000003cb48cdc9c96748afca2ce002000f79cf510b000000"].pack("H*")
server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  seen = []
  6.times do
    c = server.accept
    method = c.gets.to_s.split(" ")[0]
    headers = {}
    while (line = c.gets)
      break if line.strip.empty?
      name, value = line.split(":", 2)
      headers[name.downcase] = value.to_s.strip
    end
    n = headers["content-length"].to_i
    c.read(n) if n > 0
    seen << headers["accept-encoding"]
    c.write("HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Length: #{gzip.bytesize}\r\nConnection: close\r\n\r\n")
    c.write(gzip) unless method == "HEAD"
    c.close
  end
  seen
end

http = Net::HTTP.new("127.0.0.1", port)
["bytes=0-30", nil].each do |range|
  headers = { "Range" => range }
  r = http.get("/", headers)
  p [r.body.nil?, r.body == gzip, r["content-encoding"]]
  r = http.head("/", headers)
  p [r.body.nil?, r.body == gzip, r["content-encoding"]]
  r = http.post("/", "", headers)
  p [r.body.nil?, r.body == gzip, r["content-encoding"]]
end
p t.value
server.close
