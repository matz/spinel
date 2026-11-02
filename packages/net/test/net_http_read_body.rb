# read_body consumes the response while the request block has the connection.
# Framing changes the chunks, not their concatenation or the repeat-read rule.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
t = Thread.new do
  9.times do |i|
    c = server.accept
    while (line = c.gets)
      break if line.strip.empty?
    end
    if i / 3 == 0
      c.write("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nConnection: close\r\n\r\nhello world")
    elsif i / 3 == 1
      c.write("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n")
      c.write("5;ext=ok\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: yes\r\n\r\n")
    else
      c.write("HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nhello world")
    end
    c.close
  end
end

3.times do
  http = Net::HTTP.new("127.0.0.1", port)
  res = http.request(Net::HTTP::Get.new("/")) do |r|
    chunks = String.new
    result = r.read_body { |chunk| chunks << chunk }
    p [r.code, chunks, result.class.to_s, r.body.equal?(result)]
    begin
      r.read_body { |chunk| puts "unexpected: #{chunk}" }
    rescue IOError => e
      puts e.message
    end
  end
  p res.body.class.to_s

  http.request(Net::HTTP::Get.new("/")) do |r|
    p [r.read_body, r.read_body, r.body]
    begin
      r.read_body { |chunk| puts "unexpected: #{chunk}" }
    rescue IOError => e
      puts e.message
    end
  end
  p http.request(Net::HTTP::Get.new("/")).body
end
t.value
server.close
