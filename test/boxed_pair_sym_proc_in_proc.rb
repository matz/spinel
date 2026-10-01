Ev = Struct.new(:name)
def pick(i) = i == 0 ? [Ev.new(:a), Ev.new(:b)] : [Ev.new(:c)].each
names = proc { |i| pick(i).map(&:name) }
p names.call(0)
p names.call(1)

def pairs(i) = i == 0 ? [[1, 2], [3, 4]].each_with_index : [[5, 6]]
firsts = lambda { |i| pairs(i).map(&:first) }
p firsts.call(0)
p firsts.call(1)
lasts = proc { |i| pairs(i).map(&:last) }
p lasts.call(0)
p lasts.call(1)
