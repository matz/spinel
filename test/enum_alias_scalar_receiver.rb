# collect_concat and detect are flat_map and find under other names. An
# Integer, a String, a Symbol, a Regexp, a Time or a Proc has no
# Enumerable: a call on one is looked up by the name it was written with,
# which a top-level method reached by a send answers, or a reopening of the
# receiver's class. `private def` is a top-level method too.
private def collect_concat(x = 0)
  "top #{x}"
end
def detect
  "top detect"
end
class Symbol
  def detect = "Symbol#detect #{self}"
end
p [[1], [2]].collect_concat { |e| e }
p [[1], [2]].send(:collect_concat) { |e| e }
p 42.send(:collect_concat, 5)
p collect_concat(3)
n = 7
p n.send(:detect)
p "str".send(:detect)
p [3, 4, 5].detect { |e| e > 3 }
p (1..4).send(:detect, &:even?)
p :sym.detect
p detect
p(/x/.send(:detect))
p Time.at(0).send(:detect)
p 1.5r.send(:detect)
p StandardError.new.send(:detect)
p proc { 1 }.send(:detect)
p method(:puts).send(:detect)
p Random.new(1).send(:detect)
p Thread.new { 1 }.send(:detect)
