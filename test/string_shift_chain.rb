# chained string appends (`s << a << b`) evaluate each link exactly once and
# every append lands on the base. The tail form used to re-run the inner
# links when emitting the return value (doubling the appended text, and
# writing the doubled string back to the receiver); the value form
# wrote back only the innermost link, so the base missed the later appends.

# tail position, through a mutated parameter
def emit(buf, label)
  buf << "[" << label << "]"
end

s = +""
emit(s, "ok")
p s
r = emit(s, "go")
p r
p s

# value position: assignment keeps base and result in step
t = +"x"
u = (t << "a" << "b")
p t
p u

# parenthesized links and a longer chain
v = +""
(v << "1") << "2" << "3"
p v

# int argument appends its codepoint character
w = +"y"
w << "-" << 33
p w

# ivar base
class Sink
  def initialize
    @buf = +"<"
  end

  def push(a, b)
    @buf << a << b
  end
end

k = Sink.new
p k.push("m", ">")

# repeated tail chains accumulate (the serializer shape)
def line(out, op, arg)
  out << op << " " << arg << "\n"
end

prog = +""
line(prog, "LOAD", "a")
line(prog, "JUMP", "top")
print prog
