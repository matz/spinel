# A Method captured on a receiver is its class's own where the builtin has the name: a top-level def of the name is
# Object's private method, behind it, whether it comes before or after the capture.
m = 5.method(:succ)
p m.call
def succ = :mine
p 5.method(:succ).call
def upcase = :mine
p "s".method(:upcase).call
p 5.succ
