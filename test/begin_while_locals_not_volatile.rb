# A `begin ... end while` loop has no rescue, else or ensure, so it sets up no
# handler frame, and the locals it writes are not volatile (infer-test checks
# the C): an Integer that can be nil, a Float that can be nil, and a plain one.
# A begin with a rescue still keeps the locals it writes volatile, so a value
# written before the raise is the one the rescue sees.

class Sampler
  def initialize(freq)
    @timer = 0
    @freq = freq
  end

  def sample
    sum = 0
    v = nil
    f = nil
    n = 0
    begin
      v = -@timer
      v = @freq if v > @freq
      f = v * 0.5
      sum += v
      n += 1
      @timer += @freq
    end while @timer < 10
    [sum, v, f, n]
  end

  def guarded
    x = 0
    begin
      x = 5
      raise "boom" if @freq > 0
      x = 7
    rescue
      x += 1
    end
    x
  end
end

s = Sampler.new(3)
p s.sample
p s.guarded
p Sampler.new(0).guarded
