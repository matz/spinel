# get, post and patch hand the body to a block in pieces, or append it to a
# dest; request_get and its siblings hand the response to the block before
# its body is read. Each request here is its own connection; the server
# answers with the verb, the path and the body it got.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  loop do
    c = server.accept
    head = c.gets.to_s.split(" ")
    length = 0
    while (line = c.gets)
      break if line.strip.empty?
      name, value = line.split(":", 2)
      length = value.to_i if name.downcase == "content-length"
    end
    got = length > 0 ? c.read(length) : ""
    if head[1] == "/stop"
      c.write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
      c.close
      break
    end
    body = "#{head[0]} #{head[1]} #{got}"
    if head[0] == "HEAD"
      c.write("HTTP/1.1 200 OK\r\nContent-Length: #{body.bytesize}\r\nConnection: close\r\n\r\n")
    else
      c.write("HTTP/1.1 200 OK\r\nContent-Length: #{body.bytesize}\r\nConnection: close\r\n\r\n#{body}")
    end
    c.close
  end
end

http = Net::HTTP.new("127.0.0.1", port)

pieces = []
res = http.get("/a") { |piece| pieces << piece }
puts "get with a block: #{pieces.join.inspect}, #{res.code}"

dest = +"before "
http.get("/b", nil, dest)
puts "get with dest: #{dest.inspect}"

puts "get alone: #{http.get("/c").body.inspect}"

pieces = []
http.post("/d", "data", nil) { |piece| pieces << piece }
puts "post with a block: #{pieces.join.inspect}"

dest = +""
http.patch("/e", "more", nil, dest)
puts "patch with dest: #{dest.inspect}"

pieces = []
status = nil
http.request_get("/f") do |r|
  status = r.code
  r.read_body { |piece| pieces << piece }
end
puts "request_get: #{[status, pieces.join].inspect}"
puts "request_get alone: #{http.request_get("/g").body.inspect}"

res = http.request_head("/h") { |r| puts "request_head block: #{r.code}" }
puts "request_head: #{res.body.inspect}"

seen = ""
http.request_post("/i", "posted") { |r| seen = r.body }
puts "request_post: #{seen.inspect}"
puts "request_put: #{http.request_put("/j", "put").body.inspect}"
puts "get2: #{http.get2("/k").body.inspect}, head2: #{http.head2("/l").code}, " \
     "post2: #{http.post2("/m", "x").body.inspect}, put2: #{http.put2("/n", "y").body.inspect}"

http.get("/stop")
t.join
server.close

# The block gets a piece while the body is still coming: this server sends
# the rest only after the first piece reached the block, so a get that
# read the whole body first would wait forever.
server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
received = Queue.new
t = Thread.new do
  c = server.accept
  while (line = c.gets)
    break if line.strip.empty?
  end
  c.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n")
  c.write("a")
  received.pop
  c.write("b")
  c.close
end
total = 0
Net::HTTP.new("127.0.0.1", port).get("/") do |piece|
  received << true if total == 0 && !piece.empty?
  total += piece.bytesize
end
puts "get streams: #{total}"
t.value
server.close
