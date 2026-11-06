# A class named after another class's generated C helper (`Y__gc_scan`,
# `Y__dup`, `Y__alloc` beside Y's sp_Y__gc_scan, sp_Y__dup, sp_Y__alloc)
# had a typedef of the same name: the C did not compile. The name-clash
# pass (mc_resolve_clashes) compared helpers with methods only; a class
# whose typedef is a helper's name now takes a C name of its own. The Ruby
# names do not change.
class Y
  def initialize = @v = [1]
  attr_reader :v
end
class Y__dup; end
class Y__gc_scan; end
class Y__alloc
  def initialize(n) = @n = n
  def to_s = "alloc #{@n}"
end
y = Y.new.dup
p y.class, y.v
p Y__dup.new.class, Y__gc_scan.new.class.name
puts Y__alloc.new(3)
p [Y, Y__dup, Y__gc_scan, Y__alloc].map(&:name)
