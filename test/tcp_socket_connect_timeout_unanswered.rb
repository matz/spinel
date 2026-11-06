# spinel: linux -- a full accept queue drops the SYN on Linux, so the connect hangs; macOS refuses it
# TCPSocket.new(host, port, connect_timeout: seconds) gives up on a connect
# nothing answers after that many seconds, with IO::TimeoutError naming the
# address. A connect that drops the keyword waits as long as the kernel
# does, which is over a minute.
#
# A listener whose accept queue is full drops new SYNs, so a connect to it
# hangs as one to a dead host does, without needing a host that is down.
require "socket"

full = TCPServer.new("127.0.0.1", 0)
full.listen(1)
port = full.addr[1]

# Nothing accepts, so the queue fills and the next connects hang until the
# timeout. The kernel may take one more first; at most a few are needed.
held = []
filled = false
tries = 0
while !filled && tries < 6
  begin
    held << TCPSocket.new("127.0.0.1", port, connect_timeout: 0.3)
  rescue IO::TimeoutError
    filled = true
  end
  tries += 1
end
puts "queue full: #{filled}"

t0 = Process.clock_gettime(Process::CLOCK_MONOTONIC)
begin
  TCPSocket.new("127.0.0.1", port, connect_timeout: 0.3)
  puts "connected"
rescue IO::TimeoutError => e
  waited = Process.clock_gettime(Process::CLOCK_MONOTONIC) - t0
  puts "unanswered: #{e.class}"
  puts "names the address: #{e.message == "user specified timeout for 127.0.0.1:#{port}"}"
  puts "is an IOError: #{e.is_a?(IOError)}"
  puts "waited the timeout: #{waited >= 0.25 && waited < 5.0}"
end

# open is the same connect.
begin
  TCPSocket.open("127.0.0.1", port, connect_timeout: 0.3)
  puts "connected"
rescue IO::TimeoutError => e
  puts "open: #{e.class}"
end

held.each { |s| s.close }
full.close
