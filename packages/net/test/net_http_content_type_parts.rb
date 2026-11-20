# main_type and sub_type are the two halves of the media type, and
# type_params its parameters by name. A response with no Content-Type
# answers nil, nil and no parameters.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
types = ["text/html; charset=utf-8", "TEXT/HTML", " text/html ;q=1", "text", "text / html",
         "text/html/extra", "text/", "/html", "multipart/form-data; boundary=x; charset=utf-8",
         "application/json;charset=\"UTF-8\"", "text/plain; a=b=c", nil]
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
  p [ct, r.main_type, r.sub_type, r.type_params, r.content_type]
end
t.join
