# An `&&` or `||` whose left is a String held as its handle answers that
# handle. As an argument of a call on a receiver of more than one class, it
# fills an argument temp that holds the String's bytes, so the temp reads the
# bytes out of the handle, as a receiver of such a chain does.
class A
  def set(v)
    v
  end

  def put(v:)
    v
  end
end

class B
  def set(v)
    [v]
  end

  def put(v:)
    [v]
  end
end

def f(rec, t)
  p rec.set(t && t.upcase)
  p rec.put(v: t && t.upcase)
  p rec.put(v: t || "d")
end

f(A.new, "x" * 2)
f(B.new, nil)
