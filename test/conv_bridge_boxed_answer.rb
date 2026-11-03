# A user class's #to_int is CRuby's implicit conversion: an Integer is wanted,
# the object has one, so it is called. Spinel reaches a compiled #to_int on a
# BOXED object through a generated bridge, and the bridge only carried classes
# whose #to_int was statically an Integer. What a method answers statically is
# the analysis's business and moves with the mode -- under
# --int-overflow=promote a body of plain `1` can be typed boxed -- so the row
# was dropped and every boxed use of the object raised "no implicit conversion
# of X into Integer" instead of converting.
#
# The protocol is the other way round: call it, then judge the answer. An
# answer of the wrong kind is the TypeError, not the absence of a row.
class Ar
  def to_int
    1
  end
end
class IdxBlk
  def to_int(&b)
    1
  end
end

# through a Hash read, so the object is boxed
hA = { o: Ar.new, t: true }
p proc { |v| v }.curry(hA[:o]).call(7)

# through a mixed Array, so the element is boxed
boxed = [IdxBlk.new, "not an index"]
p [10, 20, 30][boxed[0]]

# a #to_int answering something that is not an Integer is still the TypeError,
# naming the offending answer as CRuby does
class Wrong
  def to_int
    "no"
  end
end
w = { o: Wrong.new }
begin
  p [10, 20, 30][w[:o]]
rescue TypeError => e
  puts "wrong kind: #{e.class}: #{e.message}"
end

# a class with no #to_int at all is the same TypeError
class Bare; end
bb = { o: Bare.new }
begin
  p [10, 20, 30][bb[:o]]
rescue TypeError => e
  puts "no method: #{e.message}"
end

# #to_str reaches the same bridge for a String slot
class Sr
  def to_str
    "ab"
  end
end
hs = { o: Sr.new }
p "x" + hs[:o]
