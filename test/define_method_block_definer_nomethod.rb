# A define_method or attr_* on self in a define_method's block runs on an
# object, which has no Module methods: CRuby raises NoMethodError there, and
# nothing is defined.
class K
  def initialize = (@v = 3)
  define_method(:a) { attr_reader :q; :a }
  define_method(:b) { define_method(:z) { 1 }; :b }
  define_method(:c) { |x| x ? attr_accessor(:w) : nil }
  define_method(:d) { [(attr_reader :q rescue 1), (define_method(:z) { 3 } rescue 2)] }
  define_method(:e) { l = -> { attr_writer :q; :l }; (l.call rescue $!.class) }
  define_method(:f) { |n| r = (attr_reader :v rescue n); [r, @v] }
  define_method(:g) { [1, 2].map { |i| define_method(:g1) { i } rescue i * 10 } }
  define_method(:h) { define_method(:h2) { define_method(:h3) { 1 } } }
end
class Sub < K; end

def show(obj, m, *args)
  obj.send(m, *args)
rescue NoMethodError => e
  [e.class, e.message, e.name, e.receiver.class, e.args]
end

p show(K.new, :a)
p show(Sub.new, :a)
p show(K.new, :b)
p K.new.respond_to?(:z)
p show(K.new, :c, nil)
p show(K.new, :c, 1)
p K.new.d
p K.new.e
p K.new.f(7)
p K.new.respond_to?(:v)
p K.new.g
p show(K.new, :h)
p K.new.respond_to?(:h2)

