# A wrapper hands its rest, keywords and block on to the object it holds;
# a reader that takes no arguments ignores the block
class Wrap
  def initialize(o) = @o = o
  def count_of(*args, **kw, &block) = @o.size(*args, **kw, &block)
  def key_list(*args, &block) = @o.keys(*args, &block)
  def fwd(m, *args, **kw, &block) = @o.__send__(m, *args, **kw, &block)
end
p Wrap.new("abc").count_of
p Wrap.new([1, 2]).count_of
p Wrap.new("abc").fwd(:size)
p Wrap.new([1, 2]).fwd(:length)
p Wrap.new({ a: 1 }).fwd(:empty?)
p Wrap.new({ a: 1, b: 2 }).key_list
begin
  Wrap.new("abc").count_of(1)
rescue ArgumentError => e
  p e.class
end
