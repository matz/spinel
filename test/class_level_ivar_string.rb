# A class-level ivar holding a String -- one a class body sets, or a class
# method -- is shared as an instance's is, as in CRuby: an append in the
# class body builds and reaches it, a method it is lent to appends to it,
# and a parameter a class method stores in it is the caller's String. The
# class body's append did not build, and the others appended to copies.

X = "!" * 100

class C; @x = +"k"; @x << "!"; p @x; end

def gr(x) = x << X
class D; @x = +"k"; gr(@x); p @x.size; end

class E; def self.go(x) = (@v = x; @v << "!"); end
s = +"s"; E.go(s); p s

class F
  def self.set(x) = (@v = x)
  def self.app = (@v << "?")
  def self.v = @v
end
t = +"t"; F.set(t); F.app; p t, F.v

class G; @n = +"n"; def self.grow = (@n << "!"; @n); end
p G.grow
