# A request that carries a body states Content-Length even when the body is
# empty, as CRuby's does for POST and PUT; one that does not carry a body
# sends it only for a body it was given (#7234).
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  9.times do
    c = server.accept
    length = "none"
    while (line = c.gets)
      break if line.strip.empty?
      name, value = line.split(":", 2)
      length = value.strip if name.downcase == "content-length"
    end
    n = length.to_i
    c.read(n) if n > 0
    puts "content-length=#{length}"
    c.write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
    c.close
  end
end

http = Net::HTTP.new("127.0.0.1", port)

post = Net::HTTP::Post.new("/")
post.body = ""
http.request(post)

http.request(Net::HTTP::Post.new("/"))

put = Net::HTTP::Put.new("/")
put.body = ""
http.request(put)

http.post("/", "")

own = Net::HTTP::Post.new("/")
own["Content-Length"] = "1"
own.body = "x"
http.request(own)

http.request(Net::HTTP::Get.new("/"))

http.request(Net::HTTP::Delete.new("/"))

del = Net::HTTP::Delete.new("/")
del.body = "ab"
http.request(del)

http.request(Net::HTTP::Head.new("/"))
t.join
