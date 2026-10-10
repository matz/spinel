# A request carries Accept: */* and User-Agent: Ruby unless the caller sets
# them, as CRuby's does: the defaults are on the request from its creation,
# a header the caller passes or sets replaces them, and setting one to nil
# leaves it out. The test does not look at Accept-Encoding;
# net_http_accept_encoding.rb does.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  6.times do
    c = server.accept
    seen = []
    while (line = c.gets)
      break if line.strip.empty?
      name, value = line.split(":", 2)
      n = name.downcase
      seen << "#{n}=#{value.strip}" if n == "accept" || n == "user-agent"
    end
    out = seen.sort.join(" ")
    c.write("HTTP/1.1 200 OK\r\nContent-Length: #{out.bytesize}\r\nConnection: close\r\n\r\n#{out}")
    c.close
  end
end

def run(port)
  req = Net::HTTP::Get.new("/")
  p [req["accept"], req["user-agent"]]

  http = Net::HTTP.new("127.0.0.1", port)
  p http.request(Net::HTTP::Get.new("/")).body
  p http.request(Net::HTTP::Get.new("/", "accept" => "application/json")).body
  p http.request(Net::HTTP::Post.new("/", "User-Agent" => "my-client/1.0")).body

  req = Net::HTTP::Get.new("/")
  req["Accept"] = "text/plain"
  req["user-agent"] = nil
  p http.request(req).body

  p http.get("/").body
  p Net::HTTP.get(URI("http://127.0.0.1:#{port}/"))
end

run(port)
t.join
