# FrozenError names a frozen instance of a subclass of Array, Hash or String by
# its own class, as CRuby's message does: such an instance is boxed as the
# builtin, and the runtime asks its class (sp_frozen_what).
class MyA < Array; end
class MyH < Hash; end
class MyS < String; end

def t
  yield
rescue => e
  p [e.class, e.message]
end

m = MyA.new([5]).freeze
t { m[0] = 1 }
t { m << 1 }
t { m.clear }
h = MyH.new
h[:a] = 1
h.freeze
t { h[:b] = 1 }
t { h.clear }
s = MyS.new("x").freeze
t { s << "y" }
t { s.upcase! }
