# `recv.attr op= v` on a receiver known only at run time that can be an
# object of a native class (Liquid's lexer does `ss.pos -= 1` on a scanner
# that may have come in as a String): the op-assign kept its single node,
# whose switch has an arm for a class of the program and none for a native
# one. Alone it was refused; beside a program class it raised NoMethodError.

require "strscan"

def back_one(ss)
  ss.scan(/ab/)
  ss.pos -= 1
  ss.pos
end

def scanner_for(input)
  ss = input.is_a?(StringScanner) ? input : StringScanner.new(input)
  back_one(ss)
end

p scanner_for("abc")
p scanner_for(StringScanner.new("abd"))

class Cursor
  attr_accessor :pos
  def initialize = @pos = 10
end

def step(x)
  x.pos += 2
  x.pos
end

p step(Cursor.new)
s = StringScanner.new("abcdef")
p step(s)
p s.rest

def step_value(x) = (x.pos += 1)

p step_value(Cursor.new)
p step_value(StringScanner.new("abc"))

# under defined? nothing runs and the answer is the assignment's
def probe(x) = defined?(x.pos += 1)

p probe(StringScanner.new("abc"))
p probe("abc")
t = StringScanner.new("abc")
probe(t)
p t.pos
