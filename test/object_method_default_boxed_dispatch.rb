# An Object method's parameter default, called through a dispatch on a boxed
# value: a default that reads an earlier parameter, and one whose if/elsif
# chain or rescue makes temps of its own, run in the arm the value takes.
$n = 0
def bump = ($n += 1)
class Object
  def o(x = 1, y = x + 1) = [x, y]
  def kw(a: 1, b: a * 10) = [a, b]
  def ee(x = (if is_a?(TrueClass) then 2 elsif is_a?(Hash) then 3 else 1 end)) = x
  def se(x = bump, y = x + bump) = [x, y]
  def sf(x = self.class.name, y = x.size) = [x, y]
  def rs(x = 1, y = (Integer("z") rescue x + 5)) = [x, y]
  def cs(x = 2, y = case x when 2 then :two else :other end) = [x, y]
end
def call(v) = v.o
def ck(v) = v.kw
def ce(v) = v.ee
def cse(v) = v.se
def csf(v) = v.sf
def crs(v) = v.rs
def ccs(v) = v.cs
[{}, 3, "s", true, nil, [1], :sym, 1.5].each do |v|
  p [call(v), ck(v), ce(v), cse(v), csf(v), crs(v), ccs(v)]
end
p $n
def ca(v) = v.o(7)
def cka(v) = v.kw(a: 4)
p [ca({}), ca(3), cka({}), cka(3)]
# a later default that writes an earlier parameter through a lambda
class Object; def cw(x = 1, y = (-> { x = 7 }.call)) = [x, y]; end
def c3(v) = v.cw
[nil, 1, "s"].each { |v| p c3(v) }
