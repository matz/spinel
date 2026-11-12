# spinel: gc-minor
# spinel: gc-stress
# The receiver and arguments a refused call runs ahead of its NoMethodError
# (a private top-level method on an explicit receiver, a method defined
# only later) are held in temps until the exception stages them as #receiver
# and #args. A temp is rooted unless its box is an immediate: an empty
# Array or Hash literal, a bare Array.new, a Range, a String Range and a
# Rational are boxed as new heap objects, and an allocation after them (the
# next argument's String) must not collect them. The call's `&expr` runs
# too, after the arguments and before the raise, as CRuby takes it as the
# block before it looks the method up.
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

def make_proc
  puts "make_proc ran"
  proc { 1 }
end
def arg(n) = (puts "arg #{n}"; n)
begin
  3.hidden(&make_proc)
rescue NoMethodError => e
  puts e.message
end
begin
  nil.hidden(arg(1), &make_proc)
rescue NoMethodError => e
  puts "#{e.message} #{e.args}"
end
begin
  3.hidden(&1)
rescue NoMethodError, TypeError => e
  puts "#{e.class}: #{e.message}"
end
begin
  later(arg(2), &make_proc)
rescue NoMethodError => e
  puts "#{e.message} #{e.args}"
end

def later(*a) = :l
