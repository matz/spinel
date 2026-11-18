# A proc whose body has a redo runs more than once on one call.
n = 0
pr = proc { n += 1; redo if n < 3 }
pr.call
p n
