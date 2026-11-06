# spinel: gc-stress
# A String the intern table holds (String#-@ / #dedup, and Module#name
# through it) stays alive when the program drops it: the table answers it
# again for equal contents. One too large for the allocator's size classes
# is a block of its own, and the collector freed it with the table still
# pointing at it; the next mutable String of that size took the block, and
# a dedup of equal contents then answered that String, unfrozen.
class Alpha; end
class Beta; end

def intern_once(n)
  s = -("ab" * n)
  s.frozen?
end

def name_size(k)
  k.name.size
end

def again(n)
  held = []
  8.times { held << "ab" * n }
  d = -("ab" * n)
  held.each { |m| m << "!" }
  [d.frozen?, held.any? { |m| m.equal?(d) }, d.bytesize, d == "ab" * n]
end

[20, 2000, 70000].each do |n|
  p intern_once(n)
  GC.start
  p again(n)
  GC.start
  p again(n)
end

p [Alpha, Beta].map { |k| name_size(k) }
GC.start
filler = []
64.times { |i| filler << "filler #{i}" }
p [Alpha, Beta].map { |k| k.name }
p Alpha.name.frozen?, Alpha.name.equal?(Alpha.name)
p filler.size
