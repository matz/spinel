# The source_location of a Method made on a boxed receiver is the
# position of the accessor the receiver's class answers at the bind: an
# alias's captured accessor, never a reader declared after the bind.
class A
  attr_reader :x
  alias y x
end
class B
  attr_reader :y
end
class P
  attr_reader :v
end
class Q < P; end
class R
  attr_reader :v
end

def show(s) = p(s ? [File.basename(s[0]), s[1]] : s)

show [A.new, B.new][0].method(:y).source_location
show [B.new, A.new][0].method(:y).source_location
m = [Q.new, R.new][0].method(:v)
class Q
  attr_reader :v
end
show m.source_location
