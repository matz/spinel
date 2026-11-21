# Each wait for the peer to take more of the request gets write_timeout. A
# peer that stops reading raises Net::WriteTimeout, and an idempotent request
# is sent once more on a new connection first, as after a read timeout; a
# peer that keeps reading, however slowly, gets the whole request. The
# stalled peer gives up after three seconds so a missing timeout fails
# without hanging.
require "net/http"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
p Net::HTTP.new("127.0.0.1", port).write_timeout
Net::HTTP.start("127.0.0.1", port, write_timeout: 3) { |http| p http.write_timeout }
server.close

def stalled(verb)
  server = TCPServer.new("127.0.0.1", 0)
  done = Queue.new
  held = []
  t = Thread.new do
    give_up = Process.clock_gettime(Process::CLOCK_MONOTONIC) + 3
    while done.empty? && Process.clock_gettime(Process::CLOCK_MONOTONIC) < give_up
      held << server.accept if IO.select([server], nil, nil, 0.05)
    end
    held.each(&:close)
  end
  http = Net::HTTP.new("127.0.0.1", server.addr[1])
  http.write_timeout = 0.2
  started = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  begin
    verb == "PUT" ? http.put("/", "x" * 8_000_000) : http.post("/", "x" * 8_000_000)
    puts "returned"
  rescue Net::WriteTimeout => e
    print "#{verb}: #{e.class}, a Timeout::Error: #{e.is_a?(Timeout::Error)}, "
  end
  fast = Process.clock_gettime(Process::CLOCK_MONOTONIC) - started < 1.5
  done << true
  t.value
  server.close
  puts "#{held.size} connection(s), in time: #{fast}, started after: #{http.started?}"
end

stalled("POST")
stalled("PUT")

# This peer reads 64 KB at a time and pauses between reads. Its pauses add
# up to more than write_timeout (some sixty of them), so the request gets
# through only because each wait has the timeout to itself; one wait has
# fifty times its pause, room for a loaded machine to be late in.
server = TCPServer.new("127.0.0.1", 0)
t = Thread.new do
  c = server.accept
  length = 0
  while (line = c.gets)
    break if line.strip.empty?
    name, value = line.split(":", 2)
    length = value.to_i if name.downcase == "content-length"
  end
  got = 0
  while got < length
    piece = c.readpartial(65536)
    got += piece.bytesize
    sleep 0.03
  end
  c.write("HTTP/1.1 200 OK\r\nContent-Length: #{got.to_s.bytesize}\r\nConnection: close\r\n\r\n#{got}")
  c.close
end
http = Net::HTTP.new("127.0.0.1", server.addr[1])
http.write_timeout = 1.5
puts "the slow reader got #{http.post("/", "y" * 4_000_000).body} bytes"
t.value
server.close
