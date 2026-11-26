# A class method that reads its receiving class only through a cycle of
# bare calls (b calls a, a calls b and c, c reads `name`) takes the
# receiving class all round the cycle, whichever method of it is asked
# about first.

class R
  class << self
    def c = name
    def a(n)
      b(n - 1) if n > 0
      c
    end
    def b(n) = a(n)
  end
end
class S < R; end
p S.a(1), S.b(0)
p [S.b(1), R.b(0)]

class T
  def self.y(n) = n > 0 ? x(n - 1) : to_s
  def self.x(n) = y(n)
end
class U < T; end
p U.x(2), U.y(1), T.x(0)
