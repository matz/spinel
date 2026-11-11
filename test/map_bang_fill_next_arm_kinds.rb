# A map!, collect! or fill block whose `next` arms give values of different
# kinds stores each value as it is. Joined with the tail, a `next nil` beside
# an Integer `next` read as a nullable Integer and the store raised
# TypeError; `next 1` beside `next "s"` read as boxed and the typed Array
# stayed, so the C did not build.

a = [1, 2, 3]
a.map! { |x|
  if x == 100
    next 100
  elsif x == 2
    next nil
  end
  x * 10
}
p a

b = [1, 2, 3]
b.collect! { |x| next 1 if x == 7; next "s" if x == 2; x * 10 }
p b

f = [1.5, 2.5]
f.map! { |x| next 1.0 if x > 9; next nil if x < 2; x }
p f

s = ["a", "b"]
s.map! { |x| next "z" if x == "q"; next 3 if x == "a"; x }
p s

def widen_param(arr) = arr.map! { |x| next 1 if x == 7; next nil if x == 1; x }
q = [1, 2]
widen_param(q)
p q

class Holder
  def initialize = @a = [1, 2, 3]
  def run = @a.map! { |x| next 0 if x == 9; next :sym if x == 3; x }
  attr_reader :a
end
h = Holder.new
h.run
p h.a

# fill with a block: a `next` value of another kind
c = [1, 2, 3]
c.fill { |i| next "s" if i == 1; 5 }
p c
d = [1, 2, 3]
d.fill(1) { |i| next 1 if i == 7; next nil if i == 1; 5 }
p d

# same kinds everywhere: the Array keeps its kind
e = [1, 2, 3]
e.map! { |x| next 1 if x == 7; next 2 if x == 2; x * 10 }
p e
p e.sum
