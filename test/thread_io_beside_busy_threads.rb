# Socket round trips go on while threads that never block keep the workers
# busy: the threads parked on a socket are still readied between the busy
# ones.
require "socket"

server = TCPServer.new("127.0.0.1", 0)
port = server.addr[1]
echo = Thread.new do
  s = server.accept
  while (line = s.gets)
    s.write(line)
  end
  s.close
end

done = false
busy = (0...2).map do
  Thread.new do
    n = 0
    n += 1 until done
  end
end

c = TCPSocket.new("127.0.0.1", port)
ok = 0
20.times do |i|
  c.write("ping #{i}\n")
  ok += 1 if c.gets == "ping #{i}\n"
end
c.close
done = true
busy.each(&:join)
echo.join
server.close

puts "round trips: #{ok}"
