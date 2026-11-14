# A bare call at the top level looks up main's singleton class first (the
# modules main extends), then Object's chain (a module Object includes, ahead
# of Kernel's own methods), as CRuby does.
module First
  def tag = "extend"
end
module Second
  def tag = "include"
end
module Mine
  def clone = :mine
end
extend First
Object.include(Second)
Object.include(Mine)
p tag
p clone
p 1.tag
p Object.new.clone

# a top-level def's self is its caller's: from another class's method,
# Object's include answers, not main's extend
def top = tag
class K
  def im = top
end
p K.new.im
