# Socket::Constants holds the constants Socket does: excon reads its socket
# options through it (`Socket::Constants::SOL_SOCKET`).
require "socket"

p Socket::Constants::SOL_SOCKET == Socket::SOL_SOCKET
p ::Socket::Constants::TCP_NODELAY == Socket::TCP_NODELAY
p Socket::Constants::SO_KEEPALIVE.is_a?(Integer)
p defined?(Socket::Constants::SO_KEEPALIVE)
