# A singleton def on a Struct or Data class (`def X.m`, `class << X`) is that
# class's own: the class made of it calls the hook, a send reaches it, and it
# stands before Class's methods of the same name.
Point = Struct.new(:x)
def Point.inherited(k) = puts("Point got #{k}")
class Point3 < Point; end
D = Data.define(:v)
def D.inherited(k) = puts("D got #{k}")
class DSub < D; end
Point.send(:inherited, String)
S2 = Struct.new(:y)
class << S2
  def inherited(k) = puts("S2 got #{k}")
end
class S2Sub < S2; end
module M; end
Q = Struct.new(:z)
def Q.include?(m)
  :own
end
p Q.include?(M)
p Q.send(:include?, M)
puts "done"
