# An iterator call that ends a begin inside a proc is the begin's value. It
# was stored into the begin's temp as if that were the proc's slot and the
# proc returned there: it answered nil, skipped the rest of its body and the
# ensure, and left the begin's rescue frame pushed.
p1 = proc { |a| begin; a.each { |v| v }; rescue; 0; end }
p p1.call([1, 2])

# the begin is assigned and the proc goes on
p2 = proc { |a| y = begin; a.each { |v| v }; rescue; 0; end; [y, 1] }
p p2.call([1, 2])
p3 = proc { |n| y = begin; n.times { |i| i }; rescue; 0; end; y + 1 }
p p3.call(3)

# the ensure runs and the value stays the receiver
p4 = proc { |a, x| begin; a.each { |v| v }; ensure; puts "e" if x; end }
p p4.call([3], true)
p5 = proc { |a| y = begin; a.each_with_index { |v, i| v + i }; ensure; puts "e5"; end; y.length }
p p5.call([4, 5])

# every call pops the frame it pushed
p6 = proc { |a| begin; a.each { |v| v }; rescue; 0; end }
t = 0
300.times { t += p6.call([1, 2]).length }
p t

# a lambda the same
l1 = lambda { |a| begin; a.each { |v| v }; rescue; 0; end }
p l1.call(["a"])

# the block raises, the rescue's value is the proc's
p7 = proc { |a| begin; a.each { |v| raise "r" if v > 1 }; rescue => e; e.message; end }
p p7.call([1, 2])
p p7.call([1])

# no begin around it, the proc's value as before
p8 = proc { |a| a.each { |v| v } }
p p8.call([7])
