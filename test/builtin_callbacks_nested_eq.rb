# A builtin that compares nested elements or values calls the program's
# own ==, eql? and hash: assoc and rassoc compare a pair's first or second
# element, Hash#== compares values, to_h hashes its keys. The callback
# table did not list those methods, so they were not kept and the
# builtin compared by identity.
class K
  attr_reader :v
  def initialize(v) = @v = v
  def ==(o) = o.is_a?(K) && @v == o.v
  def inspect = "I#{@v}"
end
p [[K.new(1), 5]].assoc(K.new(1))
p [[1, K.new(1)]].rassoc(K.new(1))
p({a: K.new(1)} == {a: K.new(1)}, {a: K.new(1)} != {a: K.new(1)})
p [[K.new(2)]] == [[K.new(2)]]

class H
  attr_reader :v
  def initialize(v) = @v = v
  def hash = @v.hash
  def eql?(o) = o.is_a?(H) && @v == o.v
end
a = [H.new(1), H.new(1), H.new(2)]
p a.to_h { |x| [x, 1] }.size
p a.zip(a).to_h.size
