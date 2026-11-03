# A private or protected method of a reopened builtin class raises
# NoMethodError when it is called with an explicit receiver. Self calls,
# send and a protected call from the same class still run it.
def t
  r = yield
  p r
rescue NoMethodError => e
  puts "NoMethodError: #{e.message} (#{e.receiver.inspect})"
end

class TrueClass; private def sec(x = 0) = [1, x]; protected def pro = :p; def pub = pro; end
class FalseClass; def sec(x = 0) = [:f, x]; private def fs = 9; end
class Integer
  private def isec(a) = a * 2
  protected def ipro = :ip
  def cmp(o) = o.ipro
  def me = self.isec(4)
  def bare = isec(5)
end
class NilClass; private def nsec = 8; end
class String; private def up2 = upcase; end
class Array; private; def asec = size; public; def apub = asec; end
class Hash; def hh = 1; private :hh; end
class Float; private def ff = 1.5; end
class Range; private def rr = 1; end
class Symbol; protected def sp = 1; end
class Integer; def redo = 1; private :redo; end
class Integer; def redo = 2; end
class Integer; private def pp2 = 3; public :pp2; end

t { true.sec }
t { false.sec(2) }
t { false.fs }
b = [true, false][0]
t { b.sec(3) }
t { true.pro }
t { true.pub }
t { 5.isec((puts "arg runs first"; 1)) }
t { 5.ipro }
t { 5.cmp(6) }
t { 5.me }
t { 5.bare }
t { 5.send(:isec, 3) }
t { 5.public_send(:isec, 3) }
t { nil.nsec }
t { "ab".up2 }
t { [1, 2].asec }
t { [1, 2].apub }
t { {a: 1}.hh }
t { 2.5.ff }
t { (1..2).rr }
t { :a.sp }
t { 5.redo }
t { 5.pp2 }
t { 5.respond_to?(:isec) }
t { 5.respond_to?(:isec, true) }
x = 5
t { x&.isec(1) }
v = [5, "s"].first
t { v.isec(1) }
t { [5, 6].map { |e| e.isec(1) } }
t { [1.5, 2].map { |e| e.ff rescue :err } }
