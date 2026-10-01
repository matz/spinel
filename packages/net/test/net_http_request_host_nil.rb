# A header set to nil is removed, as in CRuby: a Host set and then set to nil
# lets the default Host go out again rather than an empty one.
require "net/http"
server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  2.times do
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
g = Net::HTTP::Get.new("/")
g["Host"] = "x.example"
g["Host"] = nil
p g.key?("host")
http.request(g)
g = Net::HTTP::Get.new("/")
g["Host"] = "z.example:8080"
http.request(g)
t.join
