# #content_type is the media type without its parameters, as CRuby answers
# it: the whole `text/html; charset=utf-8` failed a check written against
# CRuby, `res.content_type == "text/html"`, for nearly every real server.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
types = ["text/html; charset=utf-8", "TEXT/HTML", " text/html ;q=1", "text", "text / html",
         "text/html/extra", "text/", "/html", nil]
t = Thread.new do
  types.each do |ct|
    c = server.accept
    while (line = c.gets)
      break if line.strip.empty?
    end
    header = ct.nil? ? "" : "Content-Type: #{ct}\r\n"
    c.write("HTTP/1.1 200 OK\r\n#{header}Content-Length: 0\r\nConnection: close\r\n\r\n")
    c.close
  end
end

types.each do |ct|
  r = Net::HTTP.new("127.0.0.1", port).get("/")
  p [ct, r.content_type]
end
t.join
