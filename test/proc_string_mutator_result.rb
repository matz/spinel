# spinel: gc-stress
g = proc { +"q" }
s = +"x"
p(g.call << s)

word = +"a"
events = []
get = proc { events << :get; word }
arg = proc { events << :arg; +"b" }
result = get.call << arg.call
p [events, result.equal?(word), word, result]

fresh = proc { +"fresh" }
collecting = proc { GC.start; +"!" }
p(fresh.call << collecting.call)
