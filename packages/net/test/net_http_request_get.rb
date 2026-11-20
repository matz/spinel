# request_get, request_head and request_post send the path, the body and the
# headers given. With a block, the response reaches the block before its body
# is read.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  9.times do
    c = server.accept
    first = c.gets.to_s.strip
    len = nil
    seen = []
    while (line = c.gets)
      break if line.strip.empty?
      name, value = line.split(":", 2)
      n = name.downcase
      len = value.strip.to_i if n == "content-length"
      seen << "#{n}=#{value.strip}" if n == "content-length" || n == "content-type" || n == "x-a"
    end
    # Reading zero bytes from an idle socket blocks in Spinel for now.
    body = len.nil? || len.zero? ? "" : c.read(len)
    out = "#{first} #{seen.sort.join(" ")} body=#{body}"
    head = "HTTP/1.1 200 OK\r\nX-Echo: #{out}\r\nContent-Length: #{out.bytesize}\r\nConnection: close\r\n\r\n"
    c.write(first.start_with?("HEAD") ? head : head + out)
    c.close
  end
end

def run(port)
  http = Net::HTTP.new("127.0.0.1", port)

  res = http.request_get("/g")
  p [res.class, res.body]
  p http.request_get("/g", { "X-A" => "1" }).body
  pieces = []
  res = http.request_get("/g") { |r| r.read_body { |piece| pieces << piece } }
  p [res.class, pieces.join]

  res = http.request_head("/h", { "X-A" => "2" })
  p [res.class, res.body, res["x-echo"]]
  seen = nil
  res = http.request_head("/h") { |r| seen = r["x-echo"] }
  p [res.class, seen]

  p http.request_post("/p", "a=1").body
  p http.request_post("/p", "{}", { "Content-Type" => "application/json" }).body
  p http.request_post("/p", "").body
  seen = nil
  res = http.request_post("/p", "x") { |r| seen = r.read_body }
  p [res.class, seen]
end

run(port)
t.join
