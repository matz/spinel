# Net::HTTP::Options and Net::HTTP::Trace are requests with no body, and
# options and trace send one with the headers given.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  5.times do
    c = server.accept
    first = c.gets.to_s.strip
    seen = []
    while (line = c.gets)
      break if line.strip.empty?
      name, value = line.split(":", 2)
      n = name.downcase
      seen << "#{n}=#{value.strip}" if n == "content-length" || n == "x-a"
    end
    out = "#{first} #{seen.sort.join(" ")}"
    c.write("HTTP/1.1 200 OK\r\nAllow: GET, OPTIONS\r\nContent-Length: #{out.bytesize}\r\nConnection: close\r\n\r\n#{out}")
    c.close
  end
end

def run(port)
  req = Net::HTTP::Options.new("/o")
  p [req.method, req.path, req.is_a?(Net::HTTPRequest)]
  p [req.request_body_permitted?, req.response_body_permitted?]
  req = Net::HTTP::Trace.new("/t")
  p [req.method, req.path, req.is_a?(Net::HTTPRequest)]
  p [req.request_body_permitted?, req.response_body_permitted?]

  http = Net::HTTP.new("127.0.0.1", port)
  res = http.options("/o")
  p [res.class, res["allow"], res.body]
  p http.options("*", { "X-A" => "1" }).body
  p http.trace("/t").body
  p http.trace("/t", { "X-A" => "2" }).body
  p http.request(Net::HTTP::Options.new("/q", "X-A" => "3")).body
end

run(port)
t.join
