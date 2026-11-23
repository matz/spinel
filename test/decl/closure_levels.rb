# closures over the same name at 1-3 levels, each stored and called later, types per level
x = 10
f1 = proc { |x| g = proc { x * 2 }; [x, g] }
v, g = f1.call(3)
p v, g.call, x
h = lambda { |x| lambda { |y| lambda { |x| [x, y] } } }
p h.(1).(2).(:three)
acc = []
[1, 2].each { |x| acc << proc { x } }
x = "changed"
p acc.map(&:call), x
