# A typed array passed to a boxed parameter (its callers disagree on the
# array kind) that the method stores a String into, through a Struct member
# that keeps the array its constructor was handed: the member widens with
# the constructor's argument, so the store reaches `src` (CRuby prints
# ["s", 2] twice). The call was refused at compile time, the store otherwise
# promoting a copy `src` never sees.
Box = Struct.new(:a)
def m(arr); arr[0] = "s"; end
m(["x"])
src = [1, 2]
x = Box.new(src).a
m(x)
p src, x
