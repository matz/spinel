# A String method's caller can take the answer's handle from the channel the
# method's reads publish. Each return has to leave that channel naming the
# String it answers: the read in a test (`ch == "q"`) published the parameter,
# and an arm that answers a new String must not hand that parameter on.

# The result slot holds a handle: an arm answers a local that holds one.
def h_tern(ch) = (ch == "q" ? (t = ch) : "Z")
def h_if(ch)
  if ch == "q" then t = ch else "Z" end
end
def h_unless(ch)
  unless ch != "q" then t = ch else "Z" end
end
def h_case(ch)
  case ch
  when "q" then t = ch
  else "Z"
  end
end
def h_return(ch)
  return (ch == "q" ? (t = ch) : "Z")
end
def h_early(ch)
  return (t = ch) if ch == "q"
  "Z"
end
def h_alias(ch); t = +"P"; t << ch; u = t; end
def h_begin(ch)
  begin
    ch == "q" ? (t = ch) : "Z"
  end
end
def h_rescue(ch)
  ch == "q" ? (t = ch) : "Z"
rescue
  "R"
end
def h_ensure(ch)
  ch == "q" ? (t = ch) : "Z"
ensure
  ch.size
end
def h_ensure_return(ch)
  begin
    return (t = ch) if ch == "q"
    "Z"
  ensure
    ch.size
  end
end
def h_proc_return(ch)
  [1].each { return (t = ch) if ch == "q" }
  "Z"
end
def h_nil(ch)
  (t = ch) if ch == "q"
end
def h_append(ch)
  t = +"P"
  return t if ch == "x"
  t << ch
end

# The result slot holds bytes.
def b_tern(ch) = (ch == "q" ? ch : "Z")
def b_case(ch)
  case ch
  when "q" then ch
  else "Z"
  end
end
def b_return(ch)
  return (ch == "q" ? ch : "Z")
end
def b_proc_return(ch)
  [1].each { return (ch == "q" ? ch : "Z") }
  "W"
end
def b_paren(ch) = (ch.size; ch == "q" ? ch : "Z")
def b_modifier(ch) = (ch if ch == "q")

out = []
"aq".each_char do |ch|
  out << h_tern(ch) << h_if(ch) << h_unless(ch) << h_case(ch)
  out << h_return(ch) << h_early(ch) << h_alias(ch) << h_begin(ch)
  out << h_rescue(ch) << h_ensure(ch) << h_ensure_return(ch) << h_proc_return(ch) << h_append(ch)
  out << b_tern(ch) << b_case(ch) << b_return(ch) << b_proc_return(ch) << b_paren(ch)
end
p out

# Each call by itself is stored, so each one takes the channel.
one = []
"aq".each_char { |ch| one << h_tern(ch) }
p one
one = []
"aq".each_char { |ch| one << h_alias(ch) }
p one
one = []
"aq".each_char { |ch| one << b_tern(ch) }
p one
p ["a", "q"].map { |ch| h_nil(ch) }
p ["a", "q"].map { |ch| b_modifier(ch) }
p [h_alias("a"), h_tern("a"), b_tern("a")]
box = [1, "s"]
p [box[1].to_s, h_tern("a")]

# An arm that answers the parameter answers that String itself.
q = +"q"
r1 = h_tern(q); r2 = h_case(q); r3 = h_early(q); r4 = b_tern(q); r5 = b_case(q)
p [r1.equal?(q), r2.equal?(q), r3.equal?(q), r4.equal?(q), r5.equal?(q)]
p [[h_tern(q)][0].equal?(q), [b_tern(q)][0].equal?(q), [h_proc_return(q)][0].equal?(q),
   [h_ensure_return(q)][0].equal?(q)]

# The same answers through an ivar, a global, a class method, a yielding
# method and a return out of Mutex#synchronize.
class Keeper
  def initialize; @t = +"i"; end
  def ivar_arm(ch) = (ch == "q" ? (@t = ch) : "Z")
  def ivar_tail(ch); @t = ch; end
  def self.class_arm(ch) = (ch == "q" ? (t = ch) : "Z")
end
$kept = +"g"
def global_arm(ch) = (ch == "q" ? ($kept = ch) : "Z")
def yielding(ch)
  r = yield ch
  r == "q" ? (t = ch) : "Z"
end
LOCK = Mutex.new
def locked_return(ch)
  LOCK.synchronize do
    return (t = ch) if ch == "q"
  end
  "Z"
end
def boxed_arm(ch) = (ch == "q" ? (t = ch) : 1)
k = Keeper.new
out = []
"aq".each_char do |ch|
  out << k.ivar_arm(ch) << Keeper.class_arm(ch) << global_arm(ch)
  out << yielding(ch) { |c| c } << locked_return(ch) << k.ivar_tail(ch)
end
p out
p ["a", "q"].map { |ch| boxed_arm(ch) }
r1 = k.ivar_arm(q); r2 = global_arm(q); r3 = locked_return(q)
p [r1.equal?(q), r2.equal?(q), r3.equal?(q)]

# A singleton override whose answer is a handle gives a copy made before
# the override the parent's boxed answer, which it publishes too.
class Lender
  def lend(ch) = (ch == "q" ? (t = ch) : "Z")
end
lender = Lender.new
early = lender.dup
def lender.lend(ch); t = +"S"; t << ch; u = t; end
one = []
"aq".each_char { |ch| one << early.lend(ch) << lender.lend(ch) }
p one
p [[early.lend(q)][0].equal?(q), [lender.lend(q)][0].equal?(q)]
