# UDPSocket#send in one thread, to a host and port and to a packed sockaddr,
# while another thread closes the socket: each sender stops on CRuby's
# IOError, never an error from a descriptor the close has let go.
require "socket"
receiver = UDPSocket.new
receiver.bind("127.0.0.1", 0)
port = receiver.addr[1]
to = Socket.sockaddr_in(port, "127.0.0.1")

sock = UDPSocket.new
p sock.send("Mapo", 0, to)
p sock.send("Mapo", 0, "127.0.0.1", port)

# Each sender says when its first send is out, or that it failed, so the
# close comes after both are sending and a first send that fails still lets
# the main thread go on.
started = Queue.new
senders = [true, false].map do |packed|
  Thread.new do
    sent = 0
    begin
      loop do
        packed ? sock.send("Yeouido", 0, to) : sock.send("Yeouido", 0, "127.0.0.1", port)
        sent += 1
        started << true if sent == 1
        Thread.pass if sent % 64 == 0
      end
    rescue IOError, SystemCallError => e
      started << false if sent == 0
      [sent > 0, e.class]
    end
  end
end
2.times { started.pop }
sleep 0.05
sock.close
senders.each { |t| p t.value }

begin
  sock.send("Mapo", 0, to)
rescue IOError => e
  p [e.class, e.message]
end

# Flags that are no C int raise before the send takes its use, so a close
# afterwards lets the descriptor go and the next socket is given it.
other = UDPSocket.new
fd = other.fileno
begin
  other.send("Mapo", 2**31, "127.0.0.1", port)
rescue RangeError => e
  p e.class
end
other.close
again = UDPSocket.new
p again.fileno == fd
again.close
receiver.close
