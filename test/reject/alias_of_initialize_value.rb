# initialize is compiled to return nothing, so the value of a call of an
# alias of it, which CRuby answers with its last value, is not there.
# spinel: reject-builtin-class: unsupported value of a call of an alias of initialize
class A
  def initialize(v) = @v = v
  attr_reader :v
end
class B < A
  alias reinit initialize
  public :reinit
end
b = B.new(1)
x = b.reinit(5)
p x, b.v
