# A request body read from an IO as it goes out (body_stream), sent with
# the Content-Length the request states or chunked in pieces of 16 KB. A
# request with neither is an ArgumentError. Setting body or body_stream
# drops the other. The server answers with what it got after the head:
# its framing for a chunked body, its size and a sum for the rest.
require "net/http"
require "stringio"

def sum_of(s)
  t = 0
  s.each_byte { |b| t = (t * 31 + b) % 1_000_003 }
  t
end

server = TCPServer.new("127.0.0.1", 0)
t = Thread.new do
  loop do
    c = server.accept
    head = c.gets.to_s.split(" ")
    length = -1
    chunked = false
    while (line = c.gets)
      break if line.strip.empty?
      name, value = line.split(":", 2)
      length = value.to_i if name.downcase == "content-length"
      chunked = true if name.downcase == "transfer-encoding"
    end
    if head[1] == "/stop"
      c.write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
      c.close
      break
    end
    answer = +""
    if chunked
      while (size_line = c.gets)
        size = size_line.to_i(16)
        answer << size.to_s(16) << " "
        c.read(size + 2)
        break if size == 0
      end
    else
      got = length > 0 ? c.read(length) : ""
      answer << "length #{length}, #{got.bytesize} bytes, sum #{sum_of(got)}"
    end
    c.write("HTTP/1.1 200 OK\r\nContent-Length: #{answer.bytesize}\r\nConnection: close\r\n\r\n#{answer}")
    c.close
  end
end
port = server.addr[1]
http = Net::HTTP.new("127.0.0.1", port)

data = Array.new(40_000) { |i| ((i * 7) % 251).chr }.join
path = "/tmp/net_http_body_stream_#{Process.pid}"
File.binwrite(path, data)
post = Net::HTTP::Post.new("/file")
post["Content-Length"] = data.bytesize.to_s
File.open(path, "rb") do |f|
  post.body_stream = f
  puts "file with content-length: #{http.request(post).body} (sent sum #{sum_of(data)})"
end
File.delete(path)

put = Net::HTTP::Put.new("/chunked")
put["Transfer-Encoding"] = "chunked"
put.body_stream = StringIO.new("a" * 40_000)
puts "chunked, the sizes of its chunks: #{http.request(put).body}"

post = Net::HTTP::Post.new("/empty")
post["Transfer-Encoding"] = "chunked"
post.body_stream = StringIO.new("")
puts "chunked and empty: #{http.request(post).body}"

post = Net::HTTP::Post.new("/none")
post.body_stream = StringIO.new("no length")
begin
  http.request(post)
  puts "sent without a length"
rescue ArgumentError => e
  puts "neither: #{e.message}"
end

post = Net::HTTP::Post.new("/swap")
post.body = "a body"
post.body_stream = StringIO.new("a stream")
p [post.body.to_s, post.body_stream.class]
post.body = "a body again"
p [post.body, post.body_stream]

http.get("/stop")
t.join
server.close
