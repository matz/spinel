# A Hash's value bound to a block parameter that is appended to, directly or
# by a method it is handed to: CRuby changes the String the Hash holds. The
# parameter bound a copy, and the Hash kept the old Strings.

h = {a: +"x", b: +"y"}
h.each_value { |v| v << "!" }
p h
h.each { |k, v| v << k.to_s }
p h
h.each_pair { |_k, v| v.upcase! }
p h

def bang(s) = (s << "!"; nil)
def via(s) = bang(s)
m = {a: +"x", b: 1}
m.each_value { |v| v << "?" if v.is_a?(String) }
p m
hv = {a: +"p", b: +"q"}
hv.each_value { |v| via(v) }
p hv
arr = [+"p", +"q"]
arr.each { |v| via(v) }
p arr

class Parts
  def initialize; @parts = {head: +"<", tail: +">"}; end
  def add(s, t) = (s << t; self)
  def run
    @parts.each_value { |v| v << "#" }
    @parts.each_value { |v| add(v, "%") }
    @parts
  end
end
p Parts.new.run
