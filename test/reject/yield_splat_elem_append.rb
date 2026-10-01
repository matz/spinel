# A String yielded through a splat from a value that is not a String
# variable, into a block parameter the block appends to: refused, as the
# block would append to a copy.
def q(a) = yield(*[a[0]])
arr = [+"f"]
q(arr) { |x| x << "9" }
p arr
