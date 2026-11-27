# A program class's own `public_method_defined?` (an options object's accessor)
# is the method a call on one of its instances reaches, not Module's
# reflection: the receiver is an object, not a class.
class Options
  def initialize(h) = @h = h
  def [](k) = @h[k]
  def public_method_defined?(name) = self[name]
  def method_defined?(name, inherit = true) = inherit ? self[name] : nil
  def respond_to?(name, include_all = false) = name.to_s.end_with?("?") || super
end
o = Options.new(bar: true, baz: false)
p o.public_method_defined?(:bar), o.public_method_defined?(:baz)
p o.method_defined?(:bar), o.method_defined?(:bar, false)
p o.send(:public_method_defined?, :bar)
p o.respond_to?(:anything?), o.respond_to?(:nope)
