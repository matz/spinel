# A class_eval block of the body's own class is a scope of its own: it reads
# and writes the body's locals, a local it makes is not the body's of the
# same name, and a define_method block in it holds the block's local.
class A
  x = 1
  class_eval do
    y = x + 1
    x += 10
    define_method(:xy) { [x, y] }
  end
  y = 100
  define_method(:outer) { [x, y] }
end
p A.new.xy, A.new.outer
