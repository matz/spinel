# A Method made, and not called, before a later include, in a class
# whose method_missing answers the name and whose respond_to_missing?,
# defined under a condition, answers it too.
module W; def b = 1; end
class L
  def method_missing(n, *r) = n == :b ? :mm : super
  if Time.now.year > 2000
    def respond_to_missing?(n, p = false) = n == :b || super
  end
end
l = L.new
mb = l.method(:b)
p mb.class, mb.name, mb.owner, l.respond_to?(:b)
class L; include W; end
p l.b
