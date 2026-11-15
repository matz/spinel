# map!.with_index on an ivar String Array with its block given as a proc:
# the proc's value is boxed and may be any class, which the typed array the
# result is written back into cannot hold. Refused; it did not build.
class Names
  def initialize; @s = ["a", "b"]; end
  def go; @s.map!.with_index(&proc { |x, i| x * (i + 1) }); @s; end
end
p Names.new.go
