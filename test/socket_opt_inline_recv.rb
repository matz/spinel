# spinel: gc-stress
# BasicSocket#setsockopt and #getsockopt on a receiver the call makes (a
# method's result) keep the socket while a Symbol or String level or
# option name is converted.
require "socket"

$peers = []
def make_sock
  a, b = UNIXSocket.pair
  $peers << b
  a
end

n = 0
50.times do
  make_sock.setsockopt(:SOCKET, :KEEPALIVE, 1)
  n += 1 if make_sock.getsockopt("SOCKET", "KEEPALIVE").int == 0
  $peers.pop(2).each(&:close)
end
p n
