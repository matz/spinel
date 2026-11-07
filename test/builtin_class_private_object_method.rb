# A builtin class or module named by a constant does not answer a private
# method of Object: CRuby raises NoMethodError for it.
class Object
  private def top = "t"
  def pub = "p"
end
class Foo; end

def try
  yield
rescue NoMethodError => e
  "NoMethodError: #{e.message}"
end

puts try { Integer.top }
puts try { ::String.top }
puts try { Kernel.top }
puts try { Comparable.top }
puts try { Foo.top }
puts try { 3.top }
# these still run
puts try { Integer.pub }
puts try { Kernel.pub }
puts try { Integer.send(:top) }
puts try { Integer.instance_eval { top } }
puts try { Integer.sqrt(16) }
puts try { Math.sqrt(4.0) }
K = Integer
puts try { K.top }
