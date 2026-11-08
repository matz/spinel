# A call with no argument on a boxed receiver of the Enumerable face raises
# NoMethodError with the empty args list, also when a program class without
# each is beside it and its own reject test holds the receiver first; the
# receiver is read once, accepted or rejected.
def t(l)
  r = yield
  puts [l, r].inspect
rescue Exception => e
  puts [l, e.class, (e.args rescue :noargs)].inspect
end
class Plain; end
$n = 0
def mkp(i) = [Plain.new, [3, 1], nil, 7][i]
def rp(i) = (($n += 1); mkp(i))
begin
  rp(0).product
rescue NoMethodError => e
  p e.class
end
t(:held_nil) { rp(2).product }
t(:held_int) { rp(3).product }
t(:held_ok) { rp(1).product }
p $n
