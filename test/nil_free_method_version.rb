# A method reading many fields that are nil until `reset` writes them:
# before reset it raises as CRuby does, after it answers the plain
# arithmetic, and a field set back to nil raises again.
class Osc
  def initialize(rate)
    @rate = rate
  end

  def reset
    @timer = 100
    @freq = 7
    @amp = 0
    @step = 0
  end

  def sample
    @timer -= @rate
    while @timer < 0
      @timer += @freq
      @step = (@step + 1) & 7
    end
    @amp = (@timer * 3 + @step) / (@rate + 1)
    @amp + @freq - @step
  end

  def clear_amp = (@amp = nil)
end

def show
  p yield
rescue => e
  puts "#{e.class}: #{e.message}"
end

o = Osc.new(30)
show { o.sample }
o.reset
5.times { show { o.sample } }
o.clear_amp
show { o.sample }
q = Osc.new(250)
q.reset
3.times { show { q.sample } }
