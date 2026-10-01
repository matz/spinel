# A destructured String element into a local that a callee mutates (a mutable
# String slot): the element is wrapped, not assigned as a const char *.
class Lib
  def self.open(name, flags = 0)
    name << "!"
    name
  end
end
class A; def open(n, c); n + c; end; end
class SB
  def initialize(box); @box = box; end
  def nb; 2; end
  def run(m)
    a, b = split(m.to_s)
    @box.open(a, b)
  end
  def split(x); [x.slice(0, nb), x.slice(nb..-1)]; end
end
puts SB.new(A.new).run("abcd")
puts SB.new(Lib).run("abcd")
