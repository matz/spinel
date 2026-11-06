# spinel: linux -- a full accept queue drops the SYN on Linux, so the connect hangs; macOS refuses it
# TCPSocket.new's connect_timeout: under threads: the connect parks its
# thread with the timeout, so the other threads run while it waits, and a
# connect nothing answers ends with IO::TimeoutError when the timeout passes.
require "socket"

# An answered connect, which another thread accepts.
server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
acceptor = Thread.new do
  peer = server.accept
  line = peer.gets
  peer.puts "answered: #{line}"
  peer.close
end
client = TCPSocket.new("127.0.0.1", port, connect_timeout: 5)
client.puts "ping"
puts client.gets
client.close
acceptor.join
server.close

# Nothing accepts on this listener, so its queue fills and the next connects
# hang until the timeout. The kernel may take one more first; at most a few
# are needed.
full = TCPServer.new("127.0.0.1", 0)
full.listen(1)
full_port = full.addr[1]
held = []
filled = false
tries = 0
while !filled && tries < 6
  begin
    held << TCPSocket.new("127.0.0.1", full_port, connect_timeout: 0.3)
  rescue IO::TimeoutError
    filled = true
  end
  tries += 1
end
puts "queue full: #{filled}"

hung = Thread.new do
  begin
    TCPSocket.new("127.0.0.1", full_port, connect_timeout: 0.5)
    "connected"
  rescue IO::TimeoutError => e
    "#{e.class}, #{e.message == "user specified timeout for 127.0.0.1:#{full_port}"}"
  end
end

# The main thread keeps running while the connect waits.
ticks = 0
while hung.alive?
  ticks += 1
  sleep 0.05
end
puts "unanswered: #{hung.value}"
puts "main ran meanwhile: #{ticks >= 3}"

held.each { |s| s.close }
full.close
