# A singleton override delegates to its parent's method for an object
# without the override (a #dup); the override's return holds the parent's
# answer, whatever its kind.

class Lender
  def lend(ch) = (ch == "q" ? (t = ch) : 1)
  def lend_nil(ch) = nil
  def lend_float(ch) = 1.5
  def lend_sym(ch) = :parent
  def lend_int(ch) = 7
  def lend_str(ch) = "p" + ch
  def lend_say(ch); puts "say #{ch}"; end
end

o = Lender.new
def o.lend(ch); t = +"S"; t << ch; u = t; end
def o.lend_nil(ch); t = +"N"; t << ch; t; end
def o.lend_float(ch); t = +"F"; t << ch; t; end
def o.lend_sym(ch); t = +"Y"; t << ch; t; end
def o.lend_int(ch); t = +"I"; t << ch; t; end
def o.lend_str(ch); t = +"T"; t << ch; t; end
def o.lend_say(ch); t = +"V"; t << ch; t; end

d = o.dup
p d.lend("a")
p d.lend("q")
p d.lend_nil("a")
p d.lend_float("a")
p d.lend_sym("a")
p d.lend_int("a")
p d.lend_str("a")
p d.lend_say("a")

p o.lend("a")
p o.lend_nil("a")
p o.lend_float("a")
p o.lend_sym("a")
p o.lend_int("a")
p o.lend_str("a")
p o.lend_say("a")

# the override's answer stays a String it can grow
w = o.lend("b")
w << "!" if w.is_a?(String)
p w
p d.lend("b").class

# an override that answers nil or an Integer over a parent's Integer, and
# one of mixed kinds over a parent that answers nil
class Counter
  def tally(n) = n * 2
  def note(n); puts "note #{n}"; end
  def find(n) = (n > 2 ? n : nil)
end
k = Counter.new
def k.tally(n); puts "tally #{n}"; end
def k.note(n) = (n > 1 ? n : "few")
def k.find(n) = n + 100
e = k.dup
p e.tally(4)
p k.tally(4)
p e.note(4)
p k.note(4)
p k.note(1)
p e.find(1)
p e.find(5)
p k.find(1)
