# A program's own Hash#empty?, key? and delete do not decide which
# exception raise(**h) / raise(cause: c) raises.
class Hash
  def empty? = true
  def key?(k) = false
  def delete(k) = :mine
end
c = RuntimeError.new("c")
begin; raise(x: 1); rescue Exception => e; p [e.class, e.message]; end
begin; raise(cause: c); rescue Exception => e; p [e.class, e.message]; end
begin; raise(**{}); rescue Exception => e; p [e.class, e.message]; end
p({a: 1}.empty?, {a: 1}.key?(:a), {a: 1}.delete(:a))
