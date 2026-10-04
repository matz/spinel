# The builtin fallback beside user-defined mutators needs a barrier per store.
class Mapper
  def map!
    yield "x"
    yield 1
  end
  def collect!
    yield "x"
    yield 1
  end
end
def wrap(x) = [x]
def maps(c)
  c.map! { |a| wrap(a) }
  c.collect! { |q| wrap(q) }
  nil
end
def churn(n) = (r = []; n.times { |i| r << [i, i.to_s] }; r.size)
(0..40).each do |k|
  churn(k)
  c = [[1, 2], [3, 4]]
  maps(c)
  p c
end
maps(Mapper.new)
maps(nil) rescue p $!.class
