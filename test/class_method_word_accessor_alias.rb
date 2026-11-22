# A class-method word in a method body that names a class-side alias of an
# accessor (`alias` or `alias_method` in `class << self`, a reader or a writer)
# makes the alias private as it does for any class method, once the body has
# run; the accessor it copies stays public. An alias a word does not name, a
# def after the alias, and a writer alias that a class method also writes
# through, answer as for any class method.
class A
  class << self
    attr_reader :tok
    alias tk tok
  end
  @tok = 1
  def self.hide = (private_class_method :tk; nil)
end
class B
  class << self
    attr_reader :tok
    alias tk tok
  end
  @tok = 1
  def self.tok = :d
  def self.hide = (private_class_method :tk; nil)
end
class W
  class << self
    attr_accessor :t
    alias_method :t2=, :t=
  end
  def self.hide = (private_class_method :t2=; nil)
  def self.peek = @t
end
class C
  class << self
    attr_reader :tok
    alias tk tok
    alias_method :tj, :tok
  end
  @tok = 3
  def self.hide = (private_class_method :tk; nil)
end
class D
  class << self
    attr_reader :tok
    alias tk tok
  end
  def self.tok = :d
  def self.hide
    private_class_method :tok
    private_class_method :tk
    nil
  end
end
A.hide
p [A.tok, (A.tk rescue $!.class), A.respond_to?(:tk), A.send(:tk)]
p [B.tok, B.tk]
B.hide
p [B.tok, (B.tk rescue $!.class)]
W.t2 = 1
W.hide
p [((W.t2 = 2) rescue $!.class), W.t, (W.t = 3), W.t, W.peek, W.send(:t2=, 4), W.t]
C.hide
p [C.tok, (C.tk rescue $!.class), C.tj, C.respond_to?(:tj)]
p [D.tok, D.tk]
D.hide
p [(D.tok rescue $!.class), (D.tk rescue $!.class)]
