# A class-method word in a method body that names a reader a `class << self`
# makes: a subclass that inherits the reader finds it private once the body has
# run, and a subclass's own reader of the same name, which the word did not
# name, stays public. A word that runs under a run-time condition is read by
# the calls after it.
class D
  class << self
    attr_reader :tok
  end
  @tok = 1
  def self.hide = (private_class_method :tok; nil)
  def self.set = (@tok = 7)
end
class E < D
  @tok = 2
end
class F < D
  class << self
    attr_reader :tok
  end
  @tok = 3
end
p [D.tok, E.tok, F.tok]
D.hide if ARGV.empty?
D.set
p [(D.tok rescue $!.class), (E.tok rescue $!.class), E.respond_to?(:tok), F.tok, F.respond_to?(:tok), D.send(:tok)]
