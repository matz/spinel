# respond_to? with an include_all only the run time knows, on a builtin
# value, where a private method of an Object reopening is asked: the flag
# decides, and a nil the slot may hold answers its own.
class Object
  private def op = 3
  protected def pr = 4
end

$log = []
def fl = ($log << :f; false)
def tr = ($log << :t; true)
def nl = nil
def rv(x) = ($log << :r; x)
def mk(f) = f ? 7 : nil
def mf(f) = f ? 2.5 : nil
def ms(f) = f ? "s" : nil

p [rv(7).respond_to?(:op, fl), rv(7).respond_to?(:op, tr), 7.respond_to?(:op, nl)]
p [7.respond_to?(:pr, fl), 7.respond_to?(:pr, tr)]
p [2.5.respond_to?(:op, fl), 2.5.respond_to?(:op, tr)]
p ["s".respond_to?(:op, fl), "s".respond_to?(:op, tr)]
p [:s.respond_to?(:op, fl), :s.respond_to?(:op, tr)]
p [[1].respond_to?(:op, fl), [1].respond_to?(:op, tr)]
p [{ a: 1 }.respond_to?(:op, fl), { a: 1 }.respond_to?(:op, tr)]
p [(1..2).respond_to?(:op, fl), (1..2).respond_to?(:op, tr)]
p [nil.respond_to?(:op, fl), nil.respond_to?(:op, tr)]
[true, false].each do |f|
  p [mk(f).respond_to?(:op, fl), mk(f).respond_to?(:op, tr), mk(f).respond_to?(:pr, tr)]
  p [mf(f).respond_to?(:op, fl), mf(f).respond_to?(:op, tr)]
  p [ms(f).respond_to?(:op, fl), ms(f).respond_to?(:op, tr)]
end
x = 7
puts "yes" if x.respond_to?(:op, tr)
puts "no" unless x.respond_to?(:op, fl)
p [x.respond_to?(:op, tr), 1, "z"]
h = {}
h[:k] = "s".respond_to?(:op, $log.size > 0)
p h
p $log
