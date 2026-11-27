# send(mesg, flags, dest_sockaddr) on a socket the program holds as a boxed
# value -- a block's or a thread's parameter, an Array element -- or through
# an instance variable, in a threaded program: each is BasicSocket#send's
# three-argument form, as on a typed receiver, and a class of the program's
# own beside a socket still takes Object#send.
require "socket"

rx = UDPSocket.new
rx.bind("127.0.0.1", 0)
port = rx.addr[1]
sa = Socket.sockaddr_in(port, "127.0.0.1")

socks = [UDPSocket.new, UDPSocket.new]
p socks.map { |s| s.send("Seongsu", 0, sa) }
p [rx.recv(32), rx.recv(32)]
msg = "Jayang" + "-dong"
socks.each { |s| s.send(msg, 0, sa) }
p [rx.recv(32), rx.recv(32)]

tx = UDPSocket.new
p Thread.new(tx) { |s| s.send("Kondae", 0, sa) }.value
p rx.recv(32)

# a relay that holds its socket and its peer's packed address, as a media
# relay does: the address is made once
class Relay
  def initialize(sock, peer)
    @sock = sock
    @peer = peer
  end

  def deliver(data) = @sock.send(data, 0, @peer)
end
relay = Relay.new(UDPSocket.new, Addrinfo.udp("127.0.0.1", port).to_sockaddr)
p Thread.new { relay.deliver("Guui") }.value
p rx.recv(32)

class Courier
  def ping(flags, dest) = "Achasan #{flags} #{dest.bytesize}"
end
p [Courier.new, socks[0]].map { |x| x.send("ping", 0, sa) }
p rx.recv(32)

workers = 4.times.map do |i|
  Thread.new(i) do |k|
    s = UDPSocket.new
    n = 0
    25.times { n += s.send("w#{k}", 0, sa) }
    s.close
    n
  end
end
p workers.map(&:value)
seen = Hash.new(0)
100.times { seen[rx.recv(16)] += 1 }
p seen.sort

[rx, tx, *socks].each(&:close)
