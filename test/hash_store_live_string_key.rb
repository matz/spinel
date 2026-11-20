# frozen_string_literal: true

def change(key)
  key << "c"
  GC.start
  +"value"
end

def key_for(range)
  range.begin
end

s = +"ab"
r = s..+"zz"
h = { "first" => "seed" }
h[key_for(r)] = change(s)
p h, h.keys.last.frozen?, h.keys.last.equal?(s)

s = +"ab"
r = s..+"zz"
h = { "first" => "seed" }
v = (h[key_for(r)] = change(s))
p h, h["abc"].equal?(v)

s = +"ab"
r = s..+"zz"
numbers = { "first" => 1 }
def number(key)
  key << "c"
  GC.start
  2
end
numbers.store(key_for(r), number(s))
p numbers
