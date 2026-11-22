# `dup` / `clone` of self inside a module's method, where the module is
# included into Object: self is any includer's instance, so the copy is of
# the run-time class's struct (with its instance variables, its
# initialize_copy and its frozen flag), not of the module's.
module Tryable
  def try(*args, **kwargs, &block)
    if args.empty? && kwargs.empty? && block_given?
      yield self
    else
      public_send(*args, **kwargs, &block)
    end
  end
  def twin = dup
  def twin_clone = clone
end
class Object
  include Tryable
end
class Foo
  attr_accessor :a, :copies
  def initialize = (@a = 1; @copies = 0)
  def initialize_copy(src)
    super
    @copies = src.copies + 1
  end
end
class Bar
  attr_accessor :b, :c
  def initialize = (@b = 2; @c = "c")
end
f = Foo.new
d = f.try(:dup)
d.a = 5
p [f.a, d.a, d.class, d.copies]
t = f.twin
t.a = 7
p [f.a, t.a, t.copies, t.twin.copies]
bar = Bar.new
w = bar.twin
p [bar.b, w.b, w.c.equal?(bar.c), w.class]
bar.freeze
p [bar.twin.frozen?, bar.twin_clone.frozen?]
p [f.try { |x| x.a + 1 }]
