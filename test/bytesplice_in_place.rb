# String#bytesplice changes the String where it lives, in each of its forms
# and on a receiver in any position, and answers it: every name holding the
# String sees the change, the receiver is read before the arguments run, and
# the errors come in CRuby's order (the span checks, then FrozenError; a nil
# receiver's NoMethodError once the arguments have run). A length-prefixed
# encoder that splices each record's length in after it is written keeps one
# buffer throughout.
# spinel: gc-stress

def show(s) = [s, s.bytesize, s.encoding.to_s]

def err
  yield
  :none
rescue => e
  [e.class, e.message]
end

# each form, through another name for the String
s = +"hello world"
alias_of_s = s
r = s.bytesplice(0, 5, "HELLO")
p r.equal?(s), alias_of_s
s.bytesplice(6..10, "WORLD")
s.bytesplice(0, 1, "jjj", 1, 1)
s.bytesplice(1..1, "xyz", 2..2)
s.bytesplice(s.bytesize, 0, "!")
p show(alias_of_s)

# a multibyte String and value, at character boundaries
u = +"héllo"
u.bytesplice(1, 2, "e")
u.bytesplice(0, 0, "日")
p show(u), u.length, u[1]
p err { u.bytesplice(1, 1, "x") }

# a binary receiver given UTF-8 text, ASCII-only and not
b = "ab".b
b.bytesplice(0, 1, "é")
p show(b)
c = "\xff\xfe".b
c.bytesplice(1, 1, "z")
p show(c), c.bytes

# the value is the String itself, or part of it
w = +"abc"
w.bytesplice(1, 1, w)
w.bytesplice(0, 2, w, 3, 2)
p show(w)

# the receiver is read first, the arguments in order
log = []
k = +"0123"
held = k
(log << :recv; k).bytesplice((log << :start; 1), (log << :len; 2), (log << :str; "--"))
p log, held

# a receiver in a conditional, an ivar, an element, a call's answer
cond = +"cond"
(cond if cond.size > 1).bytesplice(0, 1, "C")
class Box
  def initialize = @b = +"ivar"
  def go = (@b.bytesplice(0, 1, "I"); @b)
  def b = @b
end
bx = Box.new
bx.go
els = [+"one", +"two"]
els[1].bytesplice(0, 1, "T")
bx.b.bytesplice(1, 1, "V")
p cond, bx.b, els

# a receiver held as a value the compiler cannot type
def any(v) = v
x = any(+"boxed")
x = any(1) if ARGV.size > 5
x.bytesplice(0, 1, "B")
x.bytesplice(1..2, "OX")
p x

# errors: spans, then FrozenError; a nil receiver after its arguments
f = "frozen".freeze
p err { f.bytesplice(99, 1, "x") }, err { f.bytesplice(0, -1, "x") }, err { f.bytesplice(0, 1, "x") }
p err { (+"abc").bytesplice(0, 1, "x", 9, 1) }, err { (+"abc").bytesplice(5..6, "x") }
n = nil
n = +"z" if ARGV.size > 5
seen = []
p err { n.bytesplice((seen << :arg; 0), 1, "x") }, seen

# a nil in an Integer slot raises once every argument has run
nl = +"abcdef"
ran = []
p err { nl.bytesplice(1, nil, (ran << :str; "x")) }, ran, nl
ran = []
p err { nl.bytesplice(0, 1, (ran << :str; "x"), nil, (ran << :vlen; 1)) }, ran, nl

# as a statement too: every argument runs before one converts, a value's
# conversion raises inside the rescue around it, and a nil receiver raises
# before a value's #to_str runs
class Txt
  def to_str = ($ran << :to_str; "T")
end
$ran = []
st = +"abcdef"
p err { st.bytesplice(1, ($ran << :len; any(nil)), ($ran << :str; "x")); :stmt }, $ran
$ran = []
p err { st.bytesplice(($ran << :start; "1"), 1, ($ran << :str; "x")); :stmt }, $ran
$ran = []
p err { st.bytesplice(1, 1, ($ran << :str; any(nil))); :stmt }, $ran
$ran = []
p err { n.bytesplice(1, 1, ($ran << :str; Txt.new)); :stmt }, $ran
$ran = []
st.bytesplice(1, 1, ($ran << :str; Txt.new))
p st, $ran

# a byte splice that raises, caught over and over, leaves the String as it
# was and nothing behind (a Regexp key held as a value the compiler cannot
# type takes the boxed key's path)
key = any(/a/)
key = any(1) if ARGV.size > 5
caught = 0
kept = 0
500.times do |i|
  bin = +"a\xff".b
  caught += 1 if ((bin[key] = "\u00e9") rescue :caught) == :caught
  caught += 1 if ((bin.bytesplice(0, 1, "\u00e9")) rescue :caught) == :caught
  kept += 1 if bin.bytes == [0x61, 0xff]
  junk = "j" * (i % 40)
end
p caught, kept

# a length-prefixed encoder: each record after a placeholder byte, its
# length spliced in after it when it needs more than one byte
def put_record(buff, n)
  at = buff.bytesize
  buff << 42
  n.times { |i| buff << (i & 0x7F) }
  size = buff.bytesize - at - 1
  buff.setbyte(at, (size & 0x7F) | (size > 0x7F ? 0x80 : 0))
  size >>= 7
  extra = 0
  rest = size
  while rest != 0
    rest >>= 7
    extra += 1
  end
  buff.bytesplice(at + 1, 0, "*********", 0, extra)
  i = at + 1
  while size != 0
    b = size & 0x7F
    size >>= 7
    b |= 0x80 if size > 0
    buff.setbyte(i, b)
    i += 1
  end
end
buff = "".b
600.times { |k| put_record(buff, 100 + (k % 60)) }
p buff.bytesize, buff.sum, buff.byteslice(0, 6).bytes, show(buff)[2]
