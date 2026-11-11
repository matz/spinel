# `self.count ||= 0` through a writer whose parameter is an Integer?: the
# writer's C parameter holds its nil beside the value, so the literal on the
# right is lifted into that form. A nilable right side (`self.count &&= other`)
# stays in that form, so a nil reaches the writer instead of raising.
class Counter
  # @rbs @count: Integer?

  #: () -> void
  def initialize
    @count = nil
  end

  #: () -> Integer?
  def count
    @count
  end

  #: (Integer? value) -> Integer?
  def count=(value)
    @count = value
  end

  #: (Integer? other) -> Integer?
  def replace_with(other)
    self.count &&= other
  end

  #: () -> Integer
  def bump
    self.count ||= 0
    self.count = count.to_i + 1
  end
end

c = Counter.new
c.bump
c.bump
p c.count
p c.replace_with(5)
p c.count
p c.replace_with(nil)
p c.count
p c.replace_with(7)
