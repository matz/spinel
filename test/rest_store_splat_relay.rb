# A push of a rest parameter's elements into an Array parameter: the call's
# arguments for the rest are the values the Array takes, through a splat at
# or past the rest's position and through a caller that hands its own rest,
# or one element of it, on. Each shape has methods of its own, so no other
# call's values widen its Array.

def add(dst, *xs, tail)
  dst.push(*xs)
  tail
end
b = [[1], +"q"][0]
add(b, *["s", 0])
p b

def rest(r, *a) = r.push(*a)
def outer(r, *a) = rest(r, *a)
p outer([1], 6, "t")

def rest1(r, *a) = r.push(*a)
def outer1(r, *a) = rest1(r, a[1])
p outer1([1], 0, "w")

def rest3(r, *a) = r.push(*a)
def outer3(r, *a) = rest3(r, *a)
def top3(r, *a) = outer3(r, *a)
p top3([3], 7, :s)

# two calls in one statement, and a value read on
def rest4(r, *a) = r.push(*a)
def outer4(r, *a) = rest4(r, *a)
p outer4([7], "y").size + outer4([8], 9).size
