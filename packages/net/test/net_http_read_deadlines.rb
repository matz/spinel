# Each socket read gets read_timeout, including after the response has begun.
# The peer closes after two seconds so a missing timeout fails without hanging.
require "net/http"

def stalled(prefix, timeout = 0.2)
  server = TCPServer.new("127.0.0.1", 0)
  t = Thread.new do
    c = server.accept
    begin
      while (line = c.gets)
        break if line.strip.empty?
      end
      c.write(prefix)
      IO.select([c], nil, nil, 2)
    rescue Errno::ECONNRESET, Errno::EPIPE
      # A zero timeout may close before the peer starts its response.
    ensure
      c.close
    end
  end
  http = Net::HTTP.new("127.0.0.1", server.addr[1], nil)
  http.read_timeout = timeout
  http.max_retries = 0
  started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  begin
    http.get("/")
    puts "returned"
  rescue Net::ReadTimeout => e
    puts e.class
  end
  p Process.clock_gettime(Process::CLOCK_MONOTONIC) - started < 1.5
  t.value
  server.close
end

length = "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\n"
chunked = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
stalled(length)
stalled(length + "abc")
stalled(chunked)
stalled(chunked + "6\r\nabc")
# A partial line must also wait through the same reader as the body.
stalled("HTTP/1.1 200 OK\r\nContent-Len")
stalled(chunked + "0\r\nTrailer: unfinished")
stalled(length, 0)

# A total response can take longer than read_timeout when each read progresses.
[false, true].each do |chunks|
  server = TCPServer.new("127.0.0.1", 0)
  t = Thread.new do
    c = server.accept
    while (line = c.gets)
      break if line.strip.empty?
    end
    framing = chunks ? "Transfer-Encoding: chunked" : "Content-Length: 8"
    c.write("HTTP/1.1 200 OK\r\n#{framing}\r\nConnection: close\r\n\r\n")
    8.times do
      sleep 0.2
      c.write(chunks ? "1\r\nx\r\n" : "x")
    end
    c.write("0\r\n\r\n") if chunks
    c.close
  end
  http = Net::HTTP.new("127.0.0.1", server.addr[1], nil)
  http.read_timeout = 1
  http.max_retries = 0
  started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  p http.get("/").body == "xxxxxxxx"
  p Process.clock_gettime(Process::CLOCK_MONOTONIC) - started > 1
  t.value
  server.close
end
