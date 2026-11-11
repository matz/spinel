# `self.count ||= 0` through a writer whose parameter is an Integer?: the
# writer's C parameter holds its nil beside the value, so the literal on the
# right is lifted into that form. CRuby prints 2 for the two bumps.
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
