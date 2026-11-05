# A nil held in a boxed value answers respond_to? by the program's NilClass:
# a private, undefined or added method there, not nil's builtin surface.
class NilClass
  undef_method :rationalize
  private def =~(o) = nil
  private def zz = 1
  def yy = 2
end
def ia = [true, false].first
x = [nil, 1].first
p x.respond_to?(:rationalize), x.respond_to?(:=~), x.respond_to?(:=~, true)
p x.respond_to?(:zz), x.respond_to?(:zz, ia), x.respond_to?(:yy), x.respond_to?(:to_a)
p [nil, 1].map { |v| v.respond_to?(:rationalize) }
p [nil, "s"].map { |v| v.respond_to?(:=~) }
p [nil, 2.5].map { |v| [v.respond_to?(:zz), v.respond_to?(:zz, true), v.respond_to?(:yy)] }
def mk(f) = f ? 7 : nil
p mk(false).respond_to?(:rationalize), mk(true).respond_to?(:rationalize)
