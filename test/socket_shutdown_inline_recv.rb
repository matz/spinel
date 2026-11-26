# spinel: gc-stress
# BasicSocket#getsockopt and #shutdown on a receiver the call makes (a
# method's result) keep the socket while a Symbol or String option name or
# direction is converted, and a nil shutdown direction, typed or nilable
# Integer, shuts both directions down.
require "socket"

$peers = []
def make_sock
  a, b = UNIXSocket.pair
  $peers << b
  $peers.shift.close while $peers.size > 64
  a
end

# (each loop stays under a 256-descriptor limit: GC.start between loops
# closes the sockets the loop dropped)
n = 0
150.times { n += 1 if make_sock.getsockopt(:SOCKET, :TYPE).int == Socket::SOCK_STREAM }
p n
GC.start
n = 0
150.times { n += 1 if make_sock.shutdown(:WR) == 0 }
GC.start
150.times { n += 1 if make_sock.shutdown("RDWR") == 0 }
p n

how = ARGV.empty? ? nil : Socket::SHUT_WR
a, b = UNIXSocket.pair
p a.shutdown(how)
p a.read(1)
