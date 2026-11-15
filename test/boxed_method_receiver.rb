# Method#receiver on a Method read back out of a container answers the
# receiver the Method was bound to, as CRuby's does.
ms = [3, 4].map { |x| x.method(:+) }
p ms[0].receiver, ms[1].receiver
p [0.method(:+), 1].first.receiver
p [1.5, 2.5].map { |x| x.method(:+) }.first.receiver
p [:a].map { |x| x.method(:to_s) }.first.receiver
p [[1]].map { |x| x.method(:size) }.first.receiver
p ["q"].map { |x| x.method(:size) }.first.receiver
p ["w".method(:size), 1].first.receiver
p [[1, 2].method(:size), 1].first.receiver
p [{ k: 1 }.method(:size), 1].first.receiver
p [true.method(:&), 1].first.receiver
s = +"mut"
m = [s.method(:size), 1].first
s << "!"
p m.receiver, m.receiver.equal?(s)
class K; def hi = 1; end
k = K.new
p [k.method(:hi), 1].first.receiver.equal?(k)
p 3.method(:+).receiver
p [nil.method(:to_s), 1].first.receiver
p [-7.method(:abs), 1].first.receiver
