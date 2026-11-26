# spinel: reject-unsupported: (CallNode `instance_methods`)
# instance_methods(false) on a local holding a site's class lists two
# names here, whose order CRuby takes from its method table rather than
# from their definitions (`[:x, :w]` for the same two defs in a class
# body after another class defined v): the list is refused, not answered
# in definition order.
class A
  def v = 1
end

def mk
  k = Class.new { def w = 1; def x = 2 }
  k.instance_methods(false)
end
p mk, mk
