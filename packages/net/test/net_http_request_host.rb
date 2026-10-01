# A Host the request sets replaces the default one instead of going out
# beside it, whatever its spelling and however the request was built (#6767).
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  6.times do
    c = server.accept
    hosts = []
    while (line = c.gets)
      break if line.strip.empty?
      name, value = line.split(":", 2)
      hosts << (value.strip == "127.0.0.1:#{port}" ? "default" : value.strip) if name.downcase == "host"
    end
    puts "hosts=#{hosts.join(",")}"
    c.write("HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
    c.close
  end
end

http = Net::HTTP.new("127.0.0.1", port)

post = Net::HTTP::Post.new("/")
post["Host"] = "signed.example.com"
post.body = ""
http.request(post)

lower = Net::HTTP::Get.new("/")
lower["host"] = "lower.example.com"
http.request(lower)

http.request(Net::HTTP::Get.new("/", "HOST" => "initheader.example.com"))

http.post("/", "", { "Host" => "helper.example.com" })

http.request(Net::HTTP::Get.new("/"))

http.get("/")
t.join
