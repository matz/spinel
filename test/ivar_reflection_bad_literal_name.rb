# instance_variable_get / _set / _defined? with a literal name that is no
# instance variable name ("bad", :@1, :@@x, :"@a-b") raise NameError on
# every receiver, as in CRuby: a plain Object, a value whose class is known
# only at run time, a program class and a builtin value. The receiver and
# a set's value run first. A plain Object answered NoMethodError, and a
# program class's instance_variable_defined? answered false.
def t
  r = yield
  puts "ok #{r.inspect}"
rescue NoMethodError => e
  puts "NoMethodError #{e.message}"
rescue NameError => e
  puts "NameError #{e.message}"
end

class K
  def initialize; @a = 1; end
end

def side(tag, v)
  puts "run #{tag}"
  v
end

o = Object.new
x = [K.new, 1].first
k = K.new

t { o.instance_variable_get("bad") }
t { o.instance_variable_get(:bad) }
t { o.instance_variable_set("bad", 1) }
t { o.instance_variable_defined?(:bad) }
t { o.instance_variable_get(:"@1") }
t { o.instance_variable_get(:"@@x") }
t { o.instance_variable_get(:"@a-b") }
t { o.instance_variable_get(:"q\"x") }
t { x.instance_variable_get("bad") }
t { x.instance_variable_set(:bad, 2) }
t { x.instance_variable_defined?(:bad) }
t { k.instance_variable_defined?(:bad) }
t { k.instance_variable_defined?(:"@1") }
t { k.instance_variable_get(:bad) }
t { k.instance_variable_get(:"@1") }
t { k.instance_variable_set(:bad, 1) }
t { 5.instance_variable_get(:bad) }
t { 5.instance_variable_get(:"@9") }

# evaluation order: receiver, then the value, then the raise
t { side("recv", o).instance_variable_set(:bad, side("value", 1)) }
t { side("recv", k).instance_variable_set(:bad, side("value", 1)) }
t { side("recv", x).instance_variable_get(:bad) }

# valid names still answer
t { o.instance_variable_get(:@a) }
t { x.instance_variable_get(:@a) }
t { k.instance_variable_defined?(:@a) }
t { k.instance_variable_get("@a") }
t { x.instance_variable_defined?(:@zz) }
t { o.instance_variable_set(:@_ok, 3) }

# a Symbol name is NameError#name
begin
  k.instance_variable_get(:bad)
rescue NameError => e
  p e.name
end

# a set whose value is used: the raise keeps the call's own type
begin
  v = k.instance_variable_set(:"@1", 3)
  p v + 1
rescue NameError => e
  puts e.message
end
