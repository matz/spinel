# A class-method word in a method body that names a reader, writer or
# attr_accessor a `class << self` makes (a Symbol or a String name) makes the
# accessor private as it does for any class method, once the body has run,
# whatever reads it: a call, a call inside a block, a boxed or nilable
# receiver, two calls in one statement, `respond_to?`, `send`, `public_send`,
# `method` and `defined?`. The same name on the instance side stays public, and
# a word that runs before the accessor exists, or a def after it, decide as for
# any class method.
class A
  class << self
    attr_reader :tok
  end
  @tok = 1
  def self.hide = (private_class_method "tok"; nil)
end
class W
  class << self
    attr_writer :t
    attr_reader :t
  end
  def self.hide = (private_class_method :t=; nil)
end
class R
  class << self
    attr_accessor :t
  end
  def self.hide = (private_class_method :t; nil)
end
p [A.tok, A.respond_to?(:tok), defined?(A.tok)]
A.hide
p [(A.tok rescue $!.class), A.respond_to?(:tok), A.respond_to?(:tok, true), A.send(:tok),
   (A.public_send(:tok) rescue $!.class), defined?(A.tok)]
W.t = 1
W.hide
p [((W.t = 2) rescue $!.class), W.t, W.respond_to?(:t=), W.send(:t=, 3), W.t]
R.t = 5
R.hide
p [(R.t rescue $!.class), (R.t = 6), R.send(:t)]

class B
  class << self
    attr_reader :tok
  end
  @tok = 1
  def self.hide = (private_class_method :tok; nil)
  def self.read = tok
  def self.read_self = self.tok
end
class C
  def self.tok = 2
end
x = [B, C][ARGV.size]
p [x.tok, B.read]
p [B.tok, (B.hide; (B.tok rescue $!.class)), B.read, (B.read_self rescue $!.class)]
p [(x.tok rescue $!.class), x.respond_to?(:tok), [B, C].map { |k| (k.tok rescue $!.class) }]
y = ARGV.empty? ? B : nil
p [(y&.tok rescue $!.class), (y.tok rescue $!.class)]
p [B.method(:tok).call, (B.public_method(:tok) rescue $!.class)]

class G
  class << self
    attr_reader :tok
  end
  @tok = 1
  def self.hide = [1].each { private_class_method :tok }
end
p G.tok
G.hide
p [(G.tok rescue $!.class), G.respond_to?(:tok)]

# the instance side's accessor of the same name is not the class side's
class H
  attr_reader :tok
  class << self
    attr_reader :tok
  end
  @tok = 1
  def initialize = (@tok = 5)
  def self.hide = (private_class_method :tok; nil)
end
H.hide
p [(H.tok rescue $!.class), H.new.tok]

# a word that runs before the accessor exists makes the def it names private;
# the accessor made later replaces that def, and a def after the word replaces
# the accessor
class I
  def self.hide = (private_class_method :tok; nil)
  def self.tok = :defd
end
I.hide
p [(I.tok rescue $!.class)]
class I
  class << self
    attr_reader :tok
  end
  @tok = :attr
end
p [I.tok, I.respond_to?(:tok)]
class J
  class << self
    attr_reader :tok
  end
  @tok = 1
  def self.hide = (private_class_method :tok; nil)
end
J.hide
class J
  def self.tok = :d
end
p [J.tok, J.respond_to?(:tok)]
