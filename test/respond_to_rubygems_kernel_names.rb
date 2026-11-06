# Kernel's gem and gem_original_require are private methods of every object
# where rubygems is loaded, as in CRuby's default run; a generator helper's
# name is no method of Object.
class G
  def respond_to_missing?(n, priv = false) = n == :zz || super
end
g = G.new
%w[gem gem_original_require puts emit].each do |n|
  p [n, g.respond_to?(n.to_sym, true), g.respond_to?(n.to_sym)]
end
p Object.private_method_defined?(:emit)
p Object.method_defined?(:emit)
