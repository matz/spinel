# A builtin that compares nested elements or values calls the program's
# own ==, eql? and hash: assoc and rassoc compare a pair's first or second
# element, Hash#== compares values, to_h hashes its keys. The callback
# table did not list those methods, so they were not kept and the
# builtin compared by identity. Each case has its own class, so that no
# other call keeps its method.
class A
  attr_reader :v
  def initialize(v) = @v = v
  def ==(o) = o.is_a?(A) && @v == o.v
  def inspect = "A#{@v}"
end
p [[A.new(1), 5]].assoc(A.new(1))

class R
  attr_reader :v
  def initialize(v) = @v = v
  def ==(o) = o.is_a?(R) && @v == o.v
  def inspect = "R#{@v}"
end
p [[1, R.new(1)]].rassoc(R.new(1))

class E
  attr_reader :v
  def initialize(v) = @v = v
  def ==(o) = o.is_a?(E) && @v == o.v
end
p({a: E.new(1)} == {a: E.new(1)}, {a: E.new(1)} != {a: E.new(1)})

class Q
  attr_reader :v
  def initialize(v) = @v = v
  def eql?(o) = o.is_a?(Q) && @v == o.v
  def hash = @v.hash
end
p({a: Q.new(1)}.eql?({a: Q.new(1)}))

class H
  attr_reader :v
  def initialize(v) = @v = v
  def hash = @v.hash
  def eql?(o) = o.is_a?(H) && @v == o.v
end
a = [H.new(1), H.new(1), H.new(2)]
p a.to_h { |x| [x, 1] }.size
p a.zip(a).to_h.size
