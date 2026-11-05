# spinel: linux -- a full accept queue drops the SYN on Linux, so the connect hangs; macOS refuses it
# TCPSocket.new(host, port, connect_timeout: t) bounds the connect, as CRuby
# does: a connect nothing answers ends at the deadline with IO::TimeoutError
# instead of waiting as long as the kernel does (over a minute). The keyword
# used to be dropped without a word, so the deadline never applied.
require "socket"

# A listener with room answers at once, deadline or not.
roomy = TCPServer.new("127.0.0.1", 0)
s = TCPSocket.new("127.0.0.1", roomy.addr[1], connect_timeout: 5)
puts "connected: #{s.is_a?(TCPSocket)}"
# nil is no deadline, as in CRuby.
n = TCPSocket.new("127.0.0.1", roomy.addr[1], connect_timeout: nil)
puts "no deadline: #{n.is_a?(TCPSocket)}"
s.close
n.close
roomy.close

# Nothing accepts, so the queue fills and the next connects hang until the
# deadline. The kernel may take one more first; at most a few are needed.
full = TCPServer.new("127.0.0.1", 0)
full.listen(0)
port = full.addr[1]
held = []
outcome = "connected"
waited = 0.0
tries = 0
while outcome == "connected" && tries < 6
  t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
  begin
    held << TCPSocket.new("127.0.0.1", port, connect_timeout: 0.3)
  rescue IO::TimeoutError
    outcome = "timed out"
  end
  waited = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
  tries += 1
end
puts "full queue: #{outcome}"
puts "waited the deadline: #{waited >= 0.25 && waited < 2.0}"
held.each(&:close)
full.close

# Nothing listens on the port now: a refusal is still Errno::ECONNREFUSED,
# not a timeout.
begin
  TCPSocket.new("127.0.0.1", port, connect_timeout: 5)
  puts "refused: connected"
rescue Errno::ECONNREFUSED
  puts "refused: ECONNREFUSED"
rescue IO::TimeoutError
  puts "refused: timed out"
end
