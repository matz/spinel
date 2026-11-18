# A body with a redo in it runs more than once on one call: a proc's, a
# lambda's and an exit handler's.
n = 0
pr = proc { n += 1; redo if n < 3 }
pr.call
l = -> { n += 1; redo if n < 6 }
l.call
at_exit { n += 1; redo if n < 9 }
p n
