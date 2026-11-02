# An arm that raises gives the value of a conditional nothing: an Integer
# method whose other arms answer Integers answers an Integer, and its
# callers use it as one. A program's own `raise` is a method like any
# other, whose answer the arm gives.
def pick(a)
  if a == 0
    3
  else
    raise ArgumentError, "no #{a}"
  end
end
def tern(a) = a > 0 ? a * 2 : raise("neg")
def cased(a)
  case a
  when 1 then 10
  when 2 then 20
  else raise KeyError, "none"
  end
end
s = 0
1000.times { |i| s += pick(0) + tern(i + 1) + cased(i % 2 + 1) }
p s
p((pick(1) rescue $!.message))
p((tern(-1) rescue $!.message))
p((cased(3) rescue $!.class))
x = if s > 0 then s + 1 else raise "never" end
p x + 1

class Own
  def raise(m) = nil
  def val(a) = a == 0 ? 5 : raise("own")
end
p Own.new.val(0)
p Own.new.val(1)
