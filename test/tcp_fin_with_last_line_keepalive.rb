# A reader that has waited on a quiet peer still sees the peer's last line
# and then the end of the stream, when both arrive together.
#
# The server reads lines in a loop. After the first one it answers and
# finds nothing more, so its next read waits before it tries. The client
# then writes its last line and closes at once: the bytes and the FIN can
# arrive as one readiness event, and the read after the last line must
# not wait for another one.
require "socket"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
rounds = 20

reader = Thread.new do
  got = []
  rounds.times do
    s = server.accept
    lines = []
    while (line = s.gets)
      lines << line.chomp
      s.write("ok\n") if line == "first\n"
    end
    s.close
    got << lines
  end
  got
end

rounds.times do
  c = TCPSocket.new("127.0.0.1", port)
  c.write("first\n")
  c.gets
  sleep 0.002
  c.write("last\n")
  c.close
end

if reader.join(10)
  ok = reader.value.count { |l| l == %w[first last] }
  puts "#{ok} of #{rounds} connections read both lines and the end"
else
  puts "a reader stalled"
end
server.close
