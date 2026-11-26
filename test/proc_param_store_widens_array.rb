# A proc or lambda parameter that stores another kind into the Array its
# call passes widens that Array where it is built, for an optional, a
# post and a keyword parameter as for a required one, and a Hash such a
# parameter stores another key into widens the same way.

pr2 = proc { |x = nil| x << 1 }
a = [+"s"]; pr2.call(a); p a
pr3 = proc { |i, x = nil| x.push(i) }
a = [+"s"]; pr3.call(2, a); p a
lm2 = ->(x = []) { x.insert(0, 3) }
a = [+"s"]; lm2.(a); p a
lm3 = ->(x, y = 0) { x[1] = y }
a = [+"s"]; lm3[a, 4]; p a
pr5 = proc { |x, k: 0| x << k }
a = [+"s"]; pr5.call(a, k: 5); p a
pr6 = proc { |a, *r, z| z << 6 }
b = [+"t"]; pr6.call(0, 1, b); p b

# two literals with the parameter's name in one scope
c = [+"u"]; d = [+"v"]
w1 = proc { |x| x << 7 }
w2 = proc { |x| x << 8 }
w1.call(c); w2.call(d)
p [c, d]

ph = proc { |i, t = nil| t[:k] = "x" }
h = { a: 1 }
ph.call(0, h)
p h
