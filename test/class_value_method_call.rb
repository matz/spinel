# Regression: a Method bound to a class value that is not a statically-known
# constant (`self.class.method(:cm)` in a class another class inherits from,
# so self.class names no one class) resolves no callable target and stamps a
# NULL fn. Invoking it -- directly, through a poly slot, or through #to_proc --
# must raise NoMethodError rather than jump through NULL. CRuby answers these
# specific shapes, so the raise is a documented limitation; the guarantee pinned
# here is that every route raises in BOTH int-overflow modes (raise/wrap and
# --int-overflow=promote, where the poly-callable pre-arm has no legacy ABI gate
# and would otherwise call the NULL fn unconditionally).

def expect_nome(label)
  yield
  puts "#{label}: no raise"
rescue NoMethodError
  puts "#{label}: NoMethodError"
end

class ClassValueTarget
  def self.cm(a) = a
  def direct = self.class.method(:cm).call(3)
  def direct_proc = self.class.method(:cm).to_proc.call(3)
  def poly = (a = [self.class.method(:cm)]; a[0].call(3))
  def poly_proc = (a = [self.class.method(:cm)]; a[0].to_proc.call(3))
end
class ClassValueSub < ClassValueTarget; end

expect_nome("class_value_call")     { ClassValueTarget.new.direct }
expect_nome("class_value_toproc")   { ClassValueTarget.new.direct_proc }
expect_nome("class_value_poly")     { ClassValueTarget.new.poly }
expect_nome("class_value_polyproc") { ClassValueTarget.new.poly_proc }
