# Data#with on a boxed receiver (a Data of one of several classes): each
# `**hash` gives its keys, the elements run in source order after the
# receiver, and an Integer or Float member keeps nil both ways.
$log = []

Pt = Data.define(:a, :f)
Pq = Data.define(:a, :f)

def make(flag)
  flag ? Pt.new(a: 1, f: 1.5) : Pq.new(a: 2, f: 2.5)
end

def pass_h(h)
  h
end

def rcv(x)
  $log << :rcv
  x
end

def mark(v)
  $log << :"mark#{v}"
  v
end

def first_h
  $log << :first
  {a: 5}
end

x = make(true)
w = make(false)

# a nil given through `**` or as a keyword
p x.with(**pass_h({a: nil}))
p x.with(**{a: nil, f: nil})
p x.with(a: nil)
p w.with(f: nil)

# a receiver whose member is already nil
y = x.with(a: nil, f: nil)
p y
p y.with(**pass_h({}))
p y.with(**pass_h({a: 3}))
p y.with(f: 4.5)
p y.a.nil?
p y.f.nil?

# a `**hash` binds over the keys before it; a later key binds over it
p x.with(a: 7, **pass_h({a: 8}))
p x.with(**pass_h({a: 8}), a: 9)
p w.with(**pass_h({"a" => 6}))

# source order: the receiver, then each element
r = rcv(x).with(**first_h, **{f: mark(3.5)})
p r
p $log

# a key that is no Symbol nor String
begin
  x.with(**pass_h({1 => 2}))
rescue TypeError => e
  p e.message
end
