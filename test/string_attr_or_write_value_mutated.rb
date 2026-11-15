# spinel: gc-minor
# spinel: gc-stress
# The value of an attribute's `||=` or `&&=` is the String the attribute
# holds after it: a method that changes its receiver, called on that
# value, changes the attribute's String, and what the call answers is
# that String.
#
# Sections: `||=` over an attribute that holds a String, by each family of
# method that changes its receiver; over one that holds nil, where the
# right side is stored and changed; `&&=`; a method answering its receiver
# between the write and the change; the write in parentheses twice, after
# a statement, as the value of an `if` and of a `begin`, as an arm of a
# conditional (in the arm's own parentheses too, where a plain attribute
# write was wrong as well) and of `||`, the call's value kept where an
# Integer was, the call under `&.`, and the call's value dropped; the
# write from inside the class, to an inherited accessor and a module's, to
# a private one through self, to a Struct member and under `&.`; the
# String held while the call's arguments allocate; and the neighbours that
# were right already. Over an attribute that holds nil, `&&=` answers nil
# and the call raises NoMethodError.

class Box
  attr_accessor :s
  def initialize(s = +"box"); @s = s; @hid = +"hid"; end
  # the write from inside the class
  def own_or(t)
    r = (self.s ||= t).upcase!
    [r, @s, t, r.equal?(@s)]
  end
  def own_and(t)
    r = (self.s &&= t).upcase!
    [r, @s, t, r.equal?(@s), @s.equal?(t)]
  end
  def own_drop(t)
    (self.s ||= t).replace("own")
    [@s, t]
  end
  # a private accessor, written through self
  def hid_or(t)
    r = (self.hid ||= t).upcase!
    [r, @hid, t, r.equal?(@hid)]
  end
  # neighbour: an instance variable's `||=`
  def ivar_or(t)
    r = (@s ||= t).upcase!
    [r, @s, t, r.equal?(@s)]
  end

  private

  attr_accessor :hid
end

module Named
  attr_accessor :nm
end

class Crate < Box
  include Named
end

Pair = Struct.new(:m)

def fresh(n) = "f" + n.to_s

puts "== or"
t = +"tee"
o = Box.new
r = (o.s ||= t).tr!("o", "0")
p [r, o.s, t, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).upcase!
p [r, o.s, t, r.equal?(o.s)]
o = Box.new(+"Box")
r = (o.s ||= t).downcase!
p [r, o.s]
o = Box.new
r = (o.s ||= t).capitalize!
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).swapcase!
p [r, o.s]
o = Box.new
r = (o.s ||= t).sub!("b", "f")
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).sub!(/x\z/, "ck")
p [r, o.s]
o = Box.new
r = (o.s ||= t).gsub!("o", "oo")
p [r, o.s]
o = Box.new
r = (o.s ||= t).gsub!(/[bx]/) { |m| m.upcase }
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).succ!
p [r, o.s]
o = Box.new
r = (o.s ||= t).next!
p [r, o.s]
o = Box.new
r = (o.s ||= t).reverse!
p [r, o.s]
o = Box.new
r = (o.s ||= t).delete!("o")
p [r, o.s]
o = Box.new
r = (o.s ||= t).tr_s!("o", "0")
p [r, o.s]
o = Box.new
r = (o.s ||= t).chop!
p [r, o.s]
o = Box.new
r = (o.s ||= t).chomp!("x")
p [r, o.s]
o = Box.new
r = (o.s ||= t).delete_prefix!("b")
p [r, o.s]
o = Box.new
r = (o.s ||= t).delete_suffix!("x")
p [r, o.s]
o = Box.new(+" box ")
r = (o.s ||= t).strip!
p [r, o.s]
o = Box.new(+" box ")
r = (o.s ||= t).lstrip!
p [r, o.s]
o = Box.new(+" box ")
r = (o.s ||= t).rstrip!
p [r, o.s]
o = Box.new(+"boox")
r = (o.s ||= t).squeeze!
p [r, o.s]
o = Box.new
r = (o.s ||= t).replace("new")
p [r, o.s, t, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).freeze
p [r, o.s.frozen?, t.frozen?, r.equal?(o.s)]
o = Box.new
p [(o.s ||= t).freeze.frozen?, o.s.frozen?]

puts "== or, nil attribute"
o = Box.new(nil)
t = +"tee"
r = (o.s ||= t).tr!("e", "3")
p [r, o.s, t, o.s.equal?(t), r.equal?(t)]
o = Box.new(nil)
t = +"tee"
r = (o.s ||= t).replace("new")
p [r, o.s, t, o.s.equal?(t)]

puts "== and"
o = Box.new
t = +"tee"
r = (o.s &&= t).tr!("e", "3")
p [r, o.s, t, o.s.equal?(t), r.equal?(t)]
o = Box.new
t = +"tee"
r = (o.s &&= t).capitalize!
p [r, o.s, t]
o = Box.new
t = +"tee"
r = (o.s &&= t).replace("new")
p [r, o.s, t]
o = Box.new
t = +"tee"
r = (o.s &&= t).freeze
p [r, o.s.frozen?, t.frozen?]
t = +"tee"

# over an attribute that holds nil `&&=` answers nil, which has none of
# these methods
puts "== and, nil attribute"
o = Box.new(nil)
begin
  (o.s &&= t) << "!"
rescue NoMethodError, FrozenError => e
  puts e.class
end
begin
  (o.s &&= t).replace("new")
rescue NoMethodError, FrozenError => e
  puts e.class
end
begin
  (o.s &&= t).setbyte(0, 90)
rescue NoMethodError, IndexError => e
  puts e.class
end
p [o.s, t]

puts "== chains"
o = Box.new
r = (o.s ||= t).itself.tr!("o", "0")
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).to_s.tr!("o", "0")
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).concat("o").tr!("o", "0")
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).tr!("o", "0") << "!"
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).upcase!.downcase!.capitalize!
p [r, o.s, r.equal?(o.s)]

puts "== shapes"
c = t.size > 0
u = +"you"
o = Box.new
r = ((o.s ||= t)).upcase!
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (c; o.s ||= t).upcase!
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (if c then o.s ||= t else u end).upcase!
p [r, o.s, u, r.equal?(o.s)]
o = Box.new
r = begin; o.s ||= t; end.upcase!
p [r, o.s, r.equal?(o.s)]
o = Box.new
ra = (c ? o.s ||= t : u).tr!("o", "0")
p [ra, o.s, u, ra.equal?(o.s)]
o = Box.new
rb = (!c ? u : o.s ||= t).upcase!
p [rb, o.s, u, rb.equal?(o.s)]
o = Box.new
ra = (c ? (o.s ||= t) : u).tr!("o", "0")
p [ra, o.s, u, ra.equal?(o.s)]
o = Box.new
rb = (!c ? u : (o.s &&= t)).upcase!
p [rb, o.s, t, rb.equal?(o.s)]
t = +"tee"
o = Box.new
rb = (c ? (o.s = t) : u).tr!("e", "3")
p [rb, o.s, t, rb.equal?(o.s)]
t = +"tee"
o = Box.new
nothing = nil
rc = (nothing || o.s ||= t).replace("new")
p [rc, o.s, t, rc.equal?(o.s)]
o = Box.new
kept = 0
kept = (o.s ||= t).capitalize!
p [kept, o.s, kept.equal?(o.s)]
o = Box.new
r = (o.s ||= t)&.upcase!
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t)&.tr!("o", "0")
p [r, o.s]
o = Box.new
(o.s ||= t).upcase!
p [o.s, t]
o = Box.new
(o.s ||= t).tr!("o", "0")
p [o.s]
o = Box.new
(o.s ||= t).replace("new")
p [o.s, t]
o = Box.new
(o.s ||= t).upcase!.downcase!.capitalize!
p [o.s]
o = Box.new
(o.s &&= t).reverse!
p [o.s, t, o.s.equal?(t)]
t = +"tee"

puts "== other attributes"
p Box.new.own_or(+"tee")
p Box.new.own_and(+"tee")
p Box.new.own_drop(+"tee")
p Box.new.hid_or(+"tee")
k = Crate.new
r = (k.s ||= t).upcase!
p [r, k.s, t, r.equal?(k.s)]
k.nm = +"name"
r = (k.nm ||= t).upcase!
p [r, k.nm, t, r.equal?(k.nm)]
r = (k.nm &&= t).tr!("e", "3")
p [r, k.nm, t, k.nm.equal?(t)]
t = +"tee"
q = Pair.new(+"mem")
r = (q.m ||= t).upcase!
p [r, q.m, t, r.equal?(q.m)]
q = Pair.new(+"mem")
r = (q.m &&= t).tr!("e", "3")
p [r, q.m, t, q.m.equal?(t)]
t = +"tee"
q = Pair.new(+"mem")
(q.m ||= t).replace("new")
p [q.m, t]
o = Box.new
r = (o&.s ||= t).upcase!
p [r, o.s, t, r.equal?(o.s)]
o = Box.new
r = (o&.s &&= t).tr!("e", "3")
p [r, o.s, t, o.s.equal?(t)]
t = +"tee"

puts "== arguments that allocate"
o = Box.new
r = (o.s ||= t).replace(fresh(1) + fresh(2))
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s ||= fresh(3)).tr!("o", fresh(4)[0])
p [r, o.s, r.equal?(o.s)]
o = Box.new(nil)
r = (o.s ||= fresh(5) + fresh(6)).sub!(fresh(7)[0], fresh(8) + fresh(9))
p [r, o.s, r.equal?(o.s)]
o = Box.new
r = (o.s &&= fresh(10) + "x").gsub!("x") { fresh(11) + fresh(12) }
p [r, o.s, r.equal?(o.s)]
o = Box.new(nil)
(o.s ||= fresh(13) + fresh(14)).replace(fresh(15) + fresh(16))
p [o.s]

# right before this change too: the right side of the writes above is left
# as it was, a bang that changes nothing answers nil, a plain attribute
# write, an append and an insert, an instance variable's `||=`, a change
# made to a copy, a method that changes nothing, the write's value read as
# its bytes and kept in a local, and `&&=` over nil
puts "== neighbours"
p [t]
o = Box.new
r = (o.s ||= t).downcase!
p [r, o.s]
o = Box.new
t = +"tee"
r = (o.s = t).tr!("e", "3")
p [r, o.s, t, r.equal?(o.s)]
t = +"tee"
o = Box.new
r = (o.s ||= t) << "!"
p [r, o.s, t, r.equal?(o.s)]
o = Box.new
r = (o.s ||= t).insert(1, "-")
p [r, o.s]
p Box.new.ivar_or(+"tee")
o = Box.new
r = (o.s ||= t).dup.upcase!
p [r, o.s]
o = Box.new
r = (o.s ||= t).upcase
p [r, o.s]
o = Box.new
p [(o.s ||= t).size, (o.s &&= t) + "!", o.s, t]
o = Box.new
x = (o.s ||= t)
x << "?"
p [x, o.s, x.equal?(o.s)]
o = Box.new(nil)
r = (o.s &&= t)
p [r, o.s]
