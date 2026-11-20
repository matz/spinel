# Net::HTTP.post and Net::HTTP.put send the body and the headers given to
# the URI's host, and Net::HTTP.get and Net::HTTP.get_response take request
# headers after the URI.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  8.times do
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
    c.write("HTTP/1.1 200 OK\r\nContent-Length: #{out.bytesize}\r\nConnection: close\r\n\r\n#{out}")
    c.close
  end
end

def run(port)
  uri = URI("http://127.0.0.1:#{port}/r?q=1")

  res = Net::HTTP.post(uri, "a=1")
  p [res.class, res.body]
  p Net::HTTP.post(uri, "{}", { "Content-Type" => "application/json" }).body
  p Net::HTTP.post(uri, "").body
  res = Net::HTTP.put(uri, "x")
  p [res.class, res.body]
  p Net::HTTP.put(uri, "{}", { "Content-Type" => "application/json" }).body

  p Net::HTTP.get(uri)
  p Net::HTTP.get(uri, { "X-A" => "1" })
  res = Net::HTTP.get_response(uri, { "X-A" => "2" })
  p [res.class, res.body]
end

run(port)
t.join
