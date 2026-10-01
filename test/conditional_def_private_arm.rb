FLAG = ENV["SPINEL_TEST_NEVER_SET"] == "1"

class Codec
  unless FLAG
    private def tag(s) = "[#{s}]"
  else
    private def tag(s) = "<#{s}>"
  end

  unless FLAG
    protected def weight = 2
  else
    protected def weight = 1
  end

  unless FLAG
    private def each_twice(x) = 2.times { yield x * 2 }
  else
    private def each_twice(x) = 2.times { yield x }
  end

  def show(s) = tag(s)
  def score(o) = weight * 10 + o.weight
  def twice(x)
    out = []
    each_twice(x) { |v| out << v }
    out
  end
end

c = Codec.new
p c.show("a")
p c.score(Codec.new)
p c.twice(3)
p c.respond_to?(:tag)
p c.respond_to?(:weight)
begin
  c.tag("x")
rescue NoMethodError => e
  p e.message
end
begin
  c.weight
rescue NoMethodError => e
  p e.message
end
