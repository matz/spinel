# An attribute op-assign a method answers from a branch, or through a
# receiver with work behind it, returns the new value
class Counter
  attr_accessor :n

  def initialize
    @n = 1
  end

  def step(big)
    if big
      self.n *= 3
    else
      self.n += 100
    end
  end

  def maybe(f)
    unless f
      self.n -= 1
    else
      :kept
    end
  end

  def wrapped = (self.n += 2)
end

def first_inc(cs) = cs[0].n += 1

c = Counter.new
p c.step(true)
p c.step(false)
p c.maybe(false)
p c.maybe(true)
p c.wrapped
p first_inc([c])
p c.n
