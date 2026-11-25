# A `...` forwarder into a builtin of its own name, called through a
# forwarder with an optional leading parameter, is refused: the default is
# one argument more than the caller passes.
class Inner
  def center(...) = "x".center(...)
end
class Outer
  def center(width = 3, ...) = Inner.new.center(width, ...)
end
p Outer.new.center
