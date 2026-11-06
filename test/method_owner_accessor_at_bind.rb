# A Method of an accessor answers the accessor its bind site finds: a
# subclass's attr_reader declared after the bind is not its method.
class A
  attr_reader :x
  attr_writer :w
end
class B < A; end

def show(s) = p(s ? [File.basename(s[0]), s[1]] : s)

m = B.new.method(:x)
mw = B.new.method(:w=)
class B
  attr_reader :x
  attr_writer :w
end
p m.owner, mw.owner
show m.source_location
show mw.source_location
a = [m, mw, 1]
p a[0].owner, a[1].owner
n = B.new.method(:x)
p n.owner
show n.source_location
