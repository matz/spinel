# A receiverless `method(:m)` in an instance method of a class with its own
# `method` calls that method, as `obj.method(:m)` does (#10719): the
# counting override runs once for `method(:keep)` and once for
# `&method(:keep)`, whose lookup is read once, not per element. A class
# method's `method(:m)` is Class's, and a class without its own `method`
# builds the builtin Method.
$n = 0
class W
  def keep(x) = x * 2
  def method(n) = ($n += 1; super)
  def grab = method(:keep)
  def mine = [1, 2, 3].map(&method(:keep))
  def self.twice(x) = x * 2
  def self.cgrab = method(:twice)
end
w = W.new
p w.grab.call(3)
p w.mine
p $n
p W.cgrab.call(4)
p $n
class V
  def method(n) = :mine
  def grab = method(:to_s)
end
p V.new.grab
class Plain
  def keep(x) = x + 1
  def grab = method(:keep)
end
p Plain.new.grab.call(1)
