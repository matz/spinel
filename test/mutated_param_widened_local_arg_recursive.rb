# Under --int-overflow=promote an Array local of Integers turns poly once
# promote boxes the Integer locals pushed into it (after its fixpoint). A
# method that changes its parameter in place kept the parameter's Integer
# Array kind, so the call handed it a converted copy and the change never
# reached the caller's array. The parameter now widens with every call's
# argument for it, each still passed by reference. Every mode prints CRuby's
# answer.
# (a recursive method handing the parameter back to itself)
def drop(a, n)
  return nil if n == 0
  a.pop
  drop(a, n - 1)
end
k = []
v = 7
u = 8
k << v
k << u
k << v
drop(k, 2)
p k
