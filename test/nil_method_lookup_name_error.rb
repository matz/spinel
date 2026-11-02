# `recv.method(:m)` with no such method raises NameError at the lookup, and
# its message names the receiver's class, as CRuby's does: for nil, held as
# a typed nil or boxed, for true, and for a program's object. The lookup
# runs before the call it is a block argument of, so a reopened
# each_with_object does not run first. A NoMethodError "for nil" was raised
# instead, with "an instance of Object" for the typed receivers.
class W
  def keep(x, memo) = memo
end
class Foo; end

def try
  yield
rescue NameError => e
  puts "#{e.class}: #{e.message}"
end

w = nil
try { p w.method(:keep) }
try { p nil.method(:zork) }
try { p true.method(:zork) }
try { p Foo.new.method(:zork) }

class Array
  def each_with_object(m) = (puts "mine"; yield(first, m); m)
end
class H
  def initialize = (@w = nil; @arr = [3, 1])
  def run = p((@w = nil; @arr).each_with_object([], &@w.method(:keep)))
end
try { H.new.run }
