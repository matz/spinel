# A computed send reaches a top-level def -- a private method of Object -- on
# a builtin receiver too, in a program that reopens neither Object nor any
# builtin class; public_send refuses it.
def helper(x = 1) = x * 2

class Box; end

h = "hel" + "per"
p 5.send(h), "s".send(h, 3), [1].send(h), 2.5.send(h, 4)
p Box.new.send(h, 5)
p((5.public_send(h) rescue $!.class))
p(([1].public_send(h) rescue $!.class))
