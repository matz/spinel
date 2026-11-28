# A socket's calls other than send, each looping in a thread of its own
# while the main thread closes the socket: every one stops on CRuby's
# IOError, never an error from a descriptor the close has let go. A connect
# or bind to a host name resolves it before taking its use, as the lookup
# parks, so a close meanwhile lets the descriptor go and the call raises,
# instead of reaching the socket given that number next.
require "socket"
receiver = UDPSocket.new
receiver.bind("127.0.0.1", 0)
port = receiver.addr[1]
to = Socket.sockaddr_in(port, "127.0.0.1")

# Each thread says when its first call is done, or that it failed, so the
# close comes after all of them are running.
def race(sock, calls)
  started = Queue.new
  threads = calls.map do |call|
    Thread.new do
      ran = 0
      begin
        loop do
          call.call
          ran += 1
          started << true if ran == 1
          Thread.pass if ran % 64 == 0
        end
      rescue IOError, SystemCallError => e
        started << false if ran == 0
        [ran > 0, e.class]
      end
    end
  end
  calls.size.times { started.pop }
  sleep 0.05
  sock.close
  threads.map(&:value)
end

udp = UDPSocket.new
udp.connect("127.0.0.1", port)
p race(udp, [
  -> { udp.getsockopt(Socket::SOL_SOCKET, Socket::SO_RCVBUF) },
  -> { udp.setsockopt(Socket::SOL_SOCKET, Socket::SO_BROADCAST, true) },
  -> { udp.local_address },
  -> { udp.getsockname },
  -> { udp.getpeername },
  -> { udp.write_nonblock("Mapo", exception: false) }
])

# One connect per socket: two at once on one datagram socket can find it
# between peers on some systems.
udp = UDPSocket.new
p race(udp, [-> { udp.connect("127.0.0.1", port) }])
sock = Socket.new(:INET, :DGRAM)
p race(sock, [-> { sock.connect(to) }])
sock = Socket.new(:INET, :DGRAM)
p race(sock, [-> { sock.connect_nonblock(to) }])

server = TCPServer.new("127.0.0.1", 0)
p race(server, [
  -> { server.listen(8) },
  -> { server.accept_nonblock(exception: false) }
])

# The socket the next number goes to is never reached by a connect or a
# bind that was resolving its host when the close came.
connected = 0
bound = 0
8.times do
  sock = UDPSocket.new
  ready = Queue.new
  t = Thread.new do
    n = 0
    begin
      loop do
        sock.connect("localhost", port)
        n += 1
        ready << true if n == 1
      end
    rescue IOError, SystemCallError => e
      ready << false if n == 0
      e.class
    end
  end
  ready.pop
  sock.close
  other = UDPSocket.new
  p t.value
  begin
    other.getpeername
    connected += 1
  rescue Errno::ENOTCONN
  end
  other.close

  sock = UDPSocket.new
  ready = Queue.new
  t = Thread.new do
    ready << true
    begin
      sock.bind("localhost", 0)
    rescue IOError, SystemCallError => e
      e.class
    end
  end
  ready.pop
  sock.close
  other = UDPSocket.new
  r = t.value
  p r == 0 || r == IOError
  bound += 1 if other.local_address.ip_port != 0
  other.close
end
p [connected, bound]
receiver.close
