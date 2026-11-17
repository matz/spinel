# A boxed `recv.attr op= v` on nil, on a class with no accessor and on a reader-only class:
# NoMethodError names the reader `attr` first, and `attr=` only where the reader answered.

class A
  attr_accessor :n
  def initialize = @n = 1
end
class R
  attr_reader :n
  def initialize = @n = 2
end
class Z; end
[nil, A.new, R.new, Z.new].each do |o|
  begin
    o.n += 1
    p o.n
  rescue NoMethodError => e
    p e.name
  end
end
