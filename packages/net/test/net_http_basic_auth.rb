# basic_auth sets Authorization to the Basic credentials of the account and
# the password, on one line however long they are, and replaces an earlier
# one. proxy_basic_auth does the same for Proxy-Authorization.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  2.times do
    c = server.accept
    seen = []
    while (line = c.gets)
      break if line.strip.empty?
      seen << line.strip if line.downcase.include?("authorization")
    end
    out = seen.sort.join(" | ")
    c.write("HTTP/1.1 200 OK\r\nContent-Length: #{out.bytesize}\r\nConnection: close\r\n\r\n#{out}")
    c.close
  end
end

def run(port)
  req = Net::HTTP::Get.new("/")
  p req["authorization"]
  req.basic_auth("user", "pass")
  p req["authorization"]
  p req["Authorization"]
  req.basic_auth("user", "pa:ss")
  p req["authorization"]
  req.basic_auth("user", "")
  p req["authorization"]
  req.basic_auth("usuário", "maçã")
  p req["authorization"]
  req.basic_auth("a" * 40, "b" * 40)
  p req["authorization"]

  req = Net::HTTP::Post.new("/", "Authorization" => "Bearer token")
  req.basic_auth("user", "pass")
  p req["authorization"]
  p req["proxy-authorization"]
  req.proxy_basic_auth("proxy", "secret")
  p req["proxy-authorization"]

  http = Net::HTTP.new("127.0.0.1", port)
  req = Net::HTTP::Get.new("/")
  req.basic_auth("user", "pass")
  p http.request(req).body
  req.proxy_basic_auth("proxy", "secret")
  p http.request(req).body
end

run(port)
t.join
