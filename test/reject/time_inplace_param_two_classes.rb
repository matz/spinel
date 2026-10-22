# A method reached through a receiver of more than one class passes its
# arguments by value, so a Time it changes in place by #localtime cannot
# reach the caller: refused.
# spinel: reject-time-inplace: a receiver of more than one class
class A
  def shift(t) = t.localtime(3600)
end
class B
  def shift(t) = t.localtime(7200)
end

a = Time.utc(2026, 10, 15, 1, 0, 0)
[A.new, B.new].each { |o| o.shift(a) }
p a.utc_offset
