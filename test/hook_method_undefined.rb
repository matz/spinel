# undef and undef_method fire method_undefined (singleton_method_undefined for
# a singleton's), with the hook the class has at that point.
class Undef
  def a; end
  def b; end
  undef a
  def self.method_undefined(n) = puts("undefined #{n}")
  undef b
end
puts Undef.new.respond_to?(:b)

class UndefCall
  def self.method_undefined(n) = puts("call undefined #{n}")
  def a; end
  undef_method :a
end

# singleton_method_undefined is BasicObject's, which stands after Object's:
# a top-level def of that name is the hook a send reaches. method_undefined
# is Module's, which stands before: a top-level def of it is no hook. So are
# the removed pair.
def singleton_method_undefined(n) = puts("top smu #{n}")
def method_undefined(n) = puts("top mu #{n}")
class Plain; end
Plain.send(:singleton_method_undefined, :q)
Plain.send(:method_undefined, :r)
def singleton_method_removed(n) = puts("top smr #{n}")
def method_removed(n) = puts("top mr #{n}")
Plain.send(:singleton_method_removed, :s)
Plain.send(:method_removed, :t)
puts "done"
