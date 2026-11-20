# send_request sends a request of the method named, with the body and the
# headers given. A request with data states its length, one without sends no
# body, and a HEAD answers with no body.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  10.times do
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

  res = http.send_request("GET", "/g")
  p [res.class, res.body]
  p http.send_request("GET", "/g", nil, { "X-A" => "1" }).body
  res = http.send_request("HEAD", "/h")
  p [res.body, res["x-echo"]]
  p http.send_request("POST", "/p", "a=1").body
  p http.send_request("POST", "/p").body
  p http.send_request("PUT", "/u", "{}", { "Content-Type" => "application/json" }).body
  p http.send_request("PATCH", "/p", "").body
  p http.send_request("DELETE", "/d").body
  p http.send_request("OPTIONS", "/o").body
  p http.send_request("PROPFIND", "/w", "<x/>", { "Content-Type" => "text/xml" }).body
end

run(port)
t.join
