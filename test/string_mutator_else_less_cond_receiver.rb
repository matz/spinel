# spinel: gc-minor
# spinel: gc-stress
# replace, bytesplice and append_as_bytes called on the value of a
# conditional that has no else arm change the String the arm answers, and
# what the call answers is that String: the receiver is held once, as the
# String itself.
#
# Sections: each conditional without an else (if, a modifier if, unless,
# case) over a local; each kind of String the arm can answer (a parameter,
# an instance variable, a global, an attribute's read and its write, a
# local's write); the call's value kept where an Integer was; at the top
# level; arguments that allocate; arguments that run code; the String
# itself as an argument (append_as_bytes appends it as it was); an
# exception's text through `&.`, whose nil raises NoMethodError after the
# arguments ran; and what was right already: the arm not taken (nil has
# none of the three methods), a bang and an append through the same
# conditional, and the three methods where no arm is missing.

class Box
  attr_accessor :s
  def initialize(s = +"box"); @s = s; end
  def own_replace(c)
    r = (if c then @s end).replace("own")
    [r, @s, r.equal?(@s)]
  end
  def own_splice(c)
    r = (@s if c).bytesplice(0, 1, "ZZ")
    [r, @s, r.equal?(@s)]
  end
  def own_append(c)
    r = (case c when true then @s end).append_as_bytes("!")
    [r, @s, r.equal?(@s)]
  end
end

def fresh(n) = "f" + n.to_s

def conds(c)
  l = +"loc"
  r = (if c then l end).replace("new")
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (l if c).replace("new")
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (unless !c then l end).replace("new")
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (case c when true then l end).replace("new")
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (if c then l end).bytesplice(0, 1, "ZZ")
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (l if c).bytesplice(1, 2, "")
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (unless !c then l end).append_as_bytes("!")
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (case c when true then l end).append_as_bytes("!", 63)
  p [r, l, r.equal?(l)]
end

def leaves(c, par)
  r = (if c then par end).replace("par")
  p [r, par, r.equal?(par)]
  $glob = +"glob"
  r = ($glob if c).bytesplice(0, 1, "ZZ")
  p [r, $glob, r.equal?($glob)]
  o = Box.new
  r = (if c then o.s end).replace("read")
  p [r, o.s, r.equal?(o.s)]
  o = Box.new
  t = +"tee"
  r = (if c then o.s = t end).replace("set")
  p [r, o.s, t, r.equal?(t)]
  o = Box.new
  t = +"tee"
  r = (o.s = t if c).append_as_bytes("!")
  p [r, o.s, t, r.equal?(t)]
  l = +"loc"
  t = +"tee"
  r = (case c when true then l = t end).bytesplice(0, 1, "ZZ")
  p [r, l, t, r.equal?(t)]
end

def kept_where_an_integer_was(c)
  l = +"loc"
  k = 0
  k = (if c then l end).replace("new")
  p [k, l, k.equal?(l)]
  l = +"loc"
  k = 0
  k = (l if c).append_as_bytes("!")
  p [k, l, k.equal?(l)]
end

def not_taken(c)
  l = +"loc"
  begin
    (if c then l end).replace("new")
  rescue NoMethodError
    puts "NoMethodError"
  end
  begin
    r = (l if c).bytesplice(0, 1, "ZZ")
    p r
  rescue NoMethodError
    puts "NoMethodError"
  end
  begin
    r = (case c when true then l end).append_as_bytes("!")
    p r
  rescue NoMethodError
    puts "NoMethodError"
  end
  p [l]
end

def allocating(c)
  l = +"loc"
  r = (if c then l end).replace(fresh(1) + fresh(2))
  p [r, l, r.equal?(l)]
  l = fresh(3) + fresh(4)
  r = (l if c).append_as_bytes(fresh(5), fresh(6) + fresh(7))
  p [r, l, r.equal?(l)]
  l = fresh(8) + fresh(9)
  r = (case c when true then l end).bytesplice(0, 2, fresh(10) + fresh(11))
  p [r, l, r.equal?(l)]
end

def arg(x) = ($ran += 1; x)

def running(c)
  l = +"loc"
  r = (unless !c then l end).replace(arg("new"))
  p [r, l, r.equal?(l)]
  o = Box.new
  r = (if c then o.s end).replace(arg("read"))
  p [r, o.s, r.equal?(o.s)]
  o = Box.new
  r = (o.s if c).append_as_bytes(arg("!"))
  p [r, o.s, r.equal?(o.s)]
  o = Box.new
  t = +"tee"
  r = (if c then o.s = t end).bytesplice(0, 1, arg("ZZ"))
  p [r, o.s, t, r.equal?(t)]
  l = +"loc"
  t = +"tee"
  r = (if c then l = t end).replace(arg("set"))
  p [r, l, t, r.equal?(t)]
end

def itself_as_argument(c)
  l = +"loc"
  r = (l if c).append_as_bytes(l, l)
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (if c then l end).append_as_bytes(l, 33, l)
  p [r, l, r.equal?(l)]
end

def mk(c) = c ? RuntimeError.new(+"msg") : nil

def exception_text(c)
  x = mk(c)
  $ran = 0
  begin
    r = x&.message.clear
    p [r, $ran]
  rescue NoMethodError => e
    p [e.message, $ran]
  end
  begin
    r = (x&.message).clear
    p [r, $ran]
  rescue NoMethodError => e
    p [e.message, $ran]
  end
  begin
    r = x&.message.replace(arg("new"))
    p [r, $ran]
  rescue NoMethodError => e
    p [e.message, $ran]
  end
  begin
    r = x&.message.concat
    p [r, $ran]
  rescue NoMethodError => e
    p [e.message, $ran]
  end
  begin
    r = x&.message.append_as_bytes
    p [r, $ran]
  rescue NoMethodError => e
    p [e.message, $ran]
  end
  begin
    r = x&.message.bytesplice(0, 1, arg("ZZ"))
    p [r, $ran]
  rescue NoMethodError => e
    p [e.message, $ran]
  end
  begin
    r = x&.message.tr!("e", arg("E"))
    p [r, $ran]
  rescue NoMethodError => e
    p [e.message, $ran]
  end
  # a `&.` call over the text skips a nil exception and changes the text
  $ran = 0
  r = x&.message&.replace(arg("new"))
  p [r, x&.message, $ran]
  r = x&.message&.tr!("n", arg("N"))
  p [r, $ran]
  p x&.message
end

# right before this change too: a bang and an append through the same
# conditional, the three methods through a conditional with an else, on a
# variable, and on the conditional's value when it is a new String
# a `&.` call over a block's answer, whose argument runs code
def safe_nav_block_answer(c)
  l = +"la"
  r = (c ? l : nil).then { |y| y }&.bytesplice(0, 1, arg("zz"))
  p [r, l]
  r = (loop do break (c ? l : nil) end)&.tr!("a", arg("b"))
  p [r, l]
end

def neighbours(c)
  l = +"loc"
  r = (if c then l end).upcase!
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (l if c) << "!"
  p [r, l, r.equal?(l)]
  l = +"loc"
  u = +"you"
  r = (if c then l else u end).replace("new")
  p [r, l, u, r.equal?(l)]
  l = +"loc"
  r = l.replace("new")
  p [r, l, r.equal?(l)]
  l = +"loc"
  r = (if c then l + "!" end).replace("new")
  p [r, l]
end

puts "== conditionals"
conds(true)
puts "== leaves"
leaves(true, +"prm")
p Box.new.own_replace(true)
p Box.new.own_splice(true)
p Box.new.own_append(true)
puts "== kept where an Integer was"
kept_where_an_integer_was(true)
puts "== top level"
c = fresh(0).size > 0
l = +"top"
r = (if c then l end).replace("new")
p [r, l, r.equal?(l)]
l = +"top"
r = (l if c).bytesplice(0, 1, "ZZ")
p [r, l, r.equal?(l)]
o = Box.new
t = +"tee"
r = (case c when true then o.s = t end).append_as_bytes("!")
p [r, o.s, t, r.equal?(t)]
puts "== the arm not taken"
not_taken(false)
puts "== arguments that allocate"
allocating(true)
puts "== arguments that run code"
$ran = 0
running(true)
p $ran
puts "== the String itself as an argument"
itself_as_argument(true)
puts "== an exception's text through &."
exception_text(true)
exception_text(false)
puts "== a `&.` call over a block's answer"
safe_nav_block_answer(true)
safe_nav_block_answer(false)
puts "== neighbours"
neighbours(true)
