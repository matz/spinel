# Under --int-overflow=promote an Array local of Integers turns poly once
# promote boxes the Integer locals pushed into it (after its fixpoint). A
# method that changes its parameter in place kept the parameter's Integer
# Array kind, so the call handed it a converted copy and the change never
# reached the caller's array. The parameter now widens with every call's
# argument for it, each still passed by reference. Every mode prints CRuby's
# answer.
# (beside a caller whose local holds a method's Array)
def cut(a)
  a.pop
  nil
end
def mk = [1, 2, 3]
k = []
v = 7
k << v
u = 8
k << u
m = mk
cut(k)
cut(m)
p k, m
