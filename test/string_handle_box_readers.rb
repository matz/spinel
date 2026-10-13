# A String that is the shared handle (a name aliases it and changes it in
# place) is boxed as that handle, and the runtime readers that took a boxed
# String by its tag alone read it as the String it holds: a Regexp index
# with a capture number or name, and setsockopt's packed option value.
require "socket"

s = +"Hello World"
t = s
t << "!"
a = [s, 1]
p a[0][/(W)(o)/, 2], a[0][/(?<x>W)o/, "x"], a[0][/zz/, 1]
n = +"x"
m = n
m << ""
p a[0][/(?<x>W)o/, [n, 1][0]]

v = [1].pack("l")
w = v
w << ""
sock = Socket.new(Socket::AF_INET, Socket::SOCK_STREAM, 0)
o = [v, 1]
p sock.setsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY, o[0])
p sock.getsockopt(Socket::IPPROTO_TCP, Socket::TCP_NODELAY).int != 0
sock.close
