# `public_send(*args, **kwargs, &block)` (Object#try's form): the name is
# the rest's first element, and the keywords go on to the method it names,
# as without them.

class Box
  def initialize(v) = (@v = v)
  def v = @v
  def scaled(by) = @v * by
  def shifted(by, plus: 0) = @v * by + plus
  def each_v = yield(@v)

  def try(*args, **kwargs, &block)
    public_send(*args, **kwargs, &block) if respond_to?(args.first)
  end

  def call_name(*args, **kwargs)
    send(*args, **kwargs)
  end
end

b = Box.new(3)
p b.try(:v)
p b.try(:scaled, 2)
p b.try(:shifted, 2, plus: 1)
p b.try(:each_v) { |x| x + 10 }
p b.try(:nope)
p b.call_name(:shifted, 4)
p b.call_name(:shifted, 4, plus: 5)
