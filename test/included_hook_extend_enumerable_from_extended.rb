# A builtin module mixed into the including class by a hook:
# an extended hook extends its base, a class and a module, with Enumerable.
module M
  def self.extended(base)
    base.extend(Enumerable)
  end
  def each
    yield 7
    yield 8
  end
end
class A
  extend M
end
module N
  extend M
end
p A.to_a, A.sort.reverse, A.include?(8), N.to_a, N.map { |x| x * 2 }
