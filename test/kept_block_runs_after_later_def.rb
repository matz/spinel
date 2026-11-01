# A block an iterator's name hands to a program method that keeps it runs
# when the method's caller calls it, after a later def; one the method
# yields to, or calls before it returns, runs at once.
class S; end

class Box
  def initialize = (@a = [1])
  def each(&b) = (@b = b; self)
  def run = @b.call
end
x = Box.new
x.each { p S.z }

class Both
  def each(&b) = (b.call(1); @b = b)
  def again = @b.call(2)
end
y = Both.new
y.each { |i| p [i, (begin; S.z; rescue NoMethodError; :nme; end)] }

class Base; def each(&b) = ($k = b); end
class Sub < Base; def each = super; end
Sub.new.each { p [:sub, S.z] }

class Now; def each = yield(1); end
Now.new.each { p [:now, (begin; S.z; rescue NoMethodError; :nme; end)] }

class Lifted; def each(&b) = b.call(1); end
Lifted.new.each { p [:lifted, (begin; S.z; rescue NoMethodError; :nme; end)] }

def S.z = 7

x.run
y.again
$k.call
Now.new.each { p [:now, S.z] }
Lifted.new.each { p [:lifted, S.z] }
