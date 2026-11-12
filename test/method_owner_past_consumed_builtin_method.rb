# A boxed value's Method read on the spot leaves an owner read of another Method answerable
def arity_of(object)
  object.method(:to_s).arity
end
def info(m)
  "in #{m.owner}"
end
class Plain
  def go = 1
end
p arity_of(ARGV.empty? ? 1 : "x")
p arity_of(:sym)
puts info(Plain.new.method(:go))
