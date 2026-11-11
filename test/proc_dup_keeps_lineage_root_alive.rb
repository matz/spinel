# A Proc's dup keeps the lineage root Proc#== and #hash compare it by, so the
# root has to stay alive while the copy does: once the original is
# unreachable, a Proc allocated later must not land on its address and equal
# the copy.
def make_dup
  pr = proc { |x| x + 1 }
  pr.dup
end
d = make_dup
wrong = 0
junk = []
3000.times do |i|
  GC.start if i % 500 == 0
  q = proc { |x| x * i }
  wrong += 1 if d == q
  junk << q if i % 7 == 0
end
puts wrong
puts d == d.dup
puts d.call(1)
