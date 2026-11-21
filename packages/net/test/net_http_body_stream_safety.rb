# spinel: not-cruby -- on purpose (docs/limitations.md, net/http): CRuby writes whatever a body_stream gives and sends an idempotent request again with what is left of it
# A body_stream that does not match the Content-Length the request states
# is an ArgumentError, and the connection closes: with more, the extra
# bytes stay unsent (the server would take them for the start of the next
# request on the connection); with fewer, the server is not left waiting.
# A request read from a body_stream is not sent again after its connection
# fails: the stream was read as it went. Go's Transport does both; CRuby
# writes whatever the stream gives, and sends an idempotent request again
# with what is left of the stream.
require "net/http"
require "stringio"

# Each connection: the head, then everything until the client closes or
# 0.5 s of quiet; reported as the bytes after the head. Answers nothing.
server = TCPServer.new("127.0.0.1", 0)
seen = Queue.new
t = Thread.new do
  loop do
    c = server.accept
    head = +""
    while (line = c.gets)
      head << line
      break if line.strip.empty?
    end
    break if head.start_with?("STOP")
    after = +""
    while IO.select([c], nil, nil, 0.5)
      piece = begin
        c.readpartial(65536)
      rescue EOFError, Errno::ECONNRESET
        nil
      end
      break if piece.nil?
      after << piece
    end
    seen << after
    c.close
  end
end
port = server.addr[1]

def send_stream(port, verb, length, data)
  http = Net::HTTP.new("127.0.0.1", port)
  http.read_timeout = 1
  req = verb == "PUT" ? Net::HTTP::Put.new("/") : Net::HTTP::Post.new("/")
  req["Content-Length"] = length.to_s
  req.body_stream = StringIO.new(data)
  http.request(req)
  "answered"
rescue ArgumentError, EOFError, Net::ReadTimeout => e
  "#{e.class}: #{e.message}"
end

puts "content-length 5, stream of 10: #{send_stream(port, "POST", 5, "0123456789")}"
puts "  the server got #{seen.pop.inspect}"
puts "content-length 10, stream of 4: #{send_stream(port, "POST", 10, "0123")}"
puts "  the server got #{seen.pop.inspect}"
puts "PUT, no answer: #{send_stream(port, "PUT", 4, "abcd")}"
puts "  the server got #{seen.pop.inspect}, then #{seen.empty? ? "no second connection" : "a second connection"}"

s = TCPSocket.new("127.0.0.1", port)
s.write("STOP\r\n\r\n")
t.join
s.close
server.close
