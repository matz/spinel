# UNIXSocket.pair makes two connected UNIXSockets: each reads what the
# other writes, a type and a protocol may be given, socketpair is the same
# call, and the ends take a multiple assignment. Socket.pair still makes
# two Sockets.
require "socket"

def try(label)
  r = yield
  puts "#{label}: #{r.inspect}"
rescue => e
  puts "#{label}: #{e.class}: #{e.message}"
end

pr = UNIXSocket.pair
pr[1].write("hi")
try("pair") { [pr[0].class, pr[0].readpartial(2)] }
pr[0].write("back")
try("the other way") { pr[1].readpartial(4) }
pr[1].close
try("end of stream") { pr[0].read }
pr[0].close

a, b = UNIXSocket.pair(:STREAM)
b.write("x")
try("multiple assignment") { [a.class, b.class, a.readpartial(1)] }
a.close
b.close

d1, d2 = UNIXSocket.socketpair(:DGRAM, 0)
d1.send("one", 0)
d1.send("two", 0)
try("datagrams") { [d2.recv(10), d2.recv(10)] }
d1.close
d2.close

s1, s2 = Socket.pair(:UNIX, :STREAM)
s2.write("s")
try("Socket.pair") { [s1.class, s1.readpartial(1)] }
s1.close
s2.close
