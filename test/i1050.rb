# A grandchild's GC scan function must mark ivars introduced two or more
# levels up the inheritance chain. When a passthrough class (B) sits
# between the class that introduces an ivar (A) and the leaf (C), the
# leaf's scan must still walk PAST the empty middle class to mark the
# grandparent's ivar -- otherwise a GC fired while the instance is live
# frees the object that ivar holds (use-after-free / lost data). The
# object is reachable ONLY through the inherited ivar, and GC.start
# forces the collection that surfaces it (issue #1050).
class A
  def payload=(v); @payload = v; end
  def payload; @payload; end
end

class B < A   # passthrough: introduces no ivars of its own
end

class C < B
  def own=(v); @own = v; end
  def own; @own; end
end

def churn(n)
  i = 0
  s = ""
  while i < n
    s = s + i.to_s + ","
    i += 1
  end
  s.length
end

obj = C.new
obj.payload = "secret_" + 42.to_s   # grandparent ivar; only live reference
obj.own     = "kept_" + 7.to_s      # leaf's own ivar (already scanned)

GC.start
churn(8000)                          # reuse any freed slot
GC.start

puts obj.own
puts obj.payload
