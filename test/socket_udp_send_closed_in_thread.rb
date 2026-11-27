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

senders = [true, false].map do |packed|
  Thread.new do
    sent = 0
    begin
      loop do
        packed ? sock.send("Yeouido", 0, to) : sock.send("Yeouido", 0, "127.0.0.1", port)
        sent += 1
        Thread.pass if sent % 64 == 0
      end
    rescue IOError, SystemCallError => e
      [sent > 0, e.class]
    end
  end
end
sleep 0.2
sock.close
senders.each { |t| p t.value }

begin
  sock.send("Mapo", 0, to)
rescue IOError => e
  p [e.class, e.message]
end
receiver.close
