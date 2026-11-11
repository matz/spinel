# A builtin String iterator reached through a boxed receiver whose name a
# program class also defines: the dispatch keeps the String's arm, for a
# forwarded block, a literal block and a dynamic send alike.
class Walker
  def each_byte
    yield :b
    self
  end

  def each_char
    yield :c
    self
  end

  def each_codepoint
    yield :p
    self
  end

  def each_line
    yield :l
    self
  end

  def inspect = "#<Walker>"
end

def fwd_byte(t, &b) = t.each_byte(&b)
def fwd_char(t, &b) = t.each_char(&b)
def fwd_cp(t, &b) = t.each_codepoint(&b)
def fwd_line(t, &b) = t.each_line(&b)

out = []
p fwd_byte("ab") { |x| out << x }
p fwd_byte(Walker.new) { |x| out << x }
p fwd_char("hé") { |x| out << x }
p fwd_char(Walker.new) { |x| out << x }
p fwd_cp("hé") { |x| out << x }
p fwd_cp(Walker.new) { |x| out << x }
p fwd_line("a\nb") { |x| out << x }
p fwd_line(Walker.new) { |x| out << x }
p out

def lit_byte(t)
  r = []
  t.each_byte { |x| r << x }
  r
end

def lit_char(t)
  r = []
  t.each_char { |x| r << x }
  r
end

p lit_byte("xy"), lit_byte(Walker.new)
p lit_char("xy"), lit_char(Walker.new)

def via_send(t, m, &b) = t.send(m, &b)

sent = []
[:each_byte, :each_char, :each_codepoint, :each_line].each do |m|
  via_send("ok", m) { |x| sent << x }
  via_send(Walker.new, m) { |x| sent << x }
end
p sent
