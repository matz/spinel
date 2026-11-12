# spinel: gc-minor
# spinel: gc-stress
# The receiver and arguments a refused call runs ahead of its NoMethodError
# (a private top-level method on an explicit receiver, a method defined
# only later) are held in temps until the exception stages them as #receiver
# and #args. A temp is rooted unless its box is an immediate: an empty
# Array or Hash literal, a bare Array.new, a Range, a String Range and a
# Rational are boxed as new heap objects, and an allocation after them (the
# next argument's String) must not collect them.
def hidden(*a) = :h

def staged
  r = nil
  i = 0
  while i < 20
    begin
      yield i
    rescue NoMethodError => e
      r = [e.receiver, e.args]
    end
    i += 1
  end
  p r
end

staged { |i| nil.hidden([], "a" + i.to_s, [i] * 2) }
staged { |i| 3.hidden({}, "b" + i.to_s) }
staged { |i| nil.hidden(Array.new, "c" + i.to_s) }
staged { |i| nil.hidden((i..i + 2), "d" + i.to_s) }
staged { |i| nil.hidden(("e" + i.to_s)..("f" + i.to_s), "g" + i.to_s) }
staged { |i| 3.hidden(Rational(i, 3), "h" + i.to_s) }
staged { |i| (i..i + 1).hidden("k" + i.to_s, [i] * 3) }
staged { |i| nil.hidden(nil, true, :sym, 1.5, "lit", "m" + i.to_s) }

r = nil
i = 0
while i < 20
  begin
    later([], (i..i + 1), "n" + i.to_s)
  rescue NoMethodError => e
    r = e.args
  end
  i += 1
end
p r

def later(*a) = :l
