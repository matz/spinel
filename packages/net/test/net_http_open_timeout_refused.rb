# A refused connection is Errno::ECONNREFUSED with open_timeout set as
# without it. On macOS a second connect() after a refused one answers
# EISCONN, and a connect that took that for success handed back a dead
# socket that broke at the first write (Errno::EPIPE).
require "net/http"

closed = TCPServer.new("127.0.0.1", 0)
dead_port = closed.addr[1]
closed.close

[nil, 1.0].each do |limit|
  http = Net::HTTP.new("127.0.0.1", dead_port)
  http.open_timeout = limit
  begin
    http.start { |h| h.get("/") }
    puts "open_timeout #{limit.inspect}: connected"
  rescue Errno::ECONNREFUSED
    puts "open_timeout #{limit.inspect}: Errno::ECONNREFUSED"
  rescue StandardError => e
    puts "open_timeout #{limit.inspect}: #{e.class}"
  end
end

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  c = server.accept
  while (line = c.gets)
    break if line.strip.empty?
  end
  c.write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
  c.close
end
http = Net::HTTP.new("127.0.0.1", port)
http.open_timeout = 1.0
puts "a listening port with open_timeout: #{http.get("/").body}"
t.join
server.close
