# A class-method word in a method body that names a class-side reader, writer
# or alias (in a `class << self` or on `singleton_class`) that is the last
# definition of the name makes it private as it does for any class method, once
# the body has run: a call, `respond_to?` and `send` read the word's version.
# A def a method body makes replaces the name only once the body has run, and
# the alias of a def the program made keeps the def's own visibility.
class K
  def self.old = 1
  class << self
    attr_reader :token
    attr_writer :code
    alias seen old
    alias_method :seen2, :old
  end
  singleton_class.alias_method :seen3, :old
  def self.tail = 2
  class << self
    attr_reader :tail
  end
  @token = 7
  def self.hide
    private_class_method :token
    nil
  end
  def self.hide_w
    private_class_method :code=
    nil
  end
  def self.hide_a
    private_class_method :seen
    nil
  end
  def self.hide_a2
    private_class_method :seen2
    nil
  end
  def self.hide_a3
    private_class_method :seen3
    nil
  end
  def self.hide_t
    private_class_method :tail
    nil
  end
end
class Q
  class << self
    attr_reader :tk
  end
  def self.hide_q
    private_class_method :tk
    nil
  end
  def self.redef
    def self.tk = :defd
    nil
  end
end
p [K.token, K.respond_to?(:token), K.seen, K.seen2, K.seen3, K.tail, Q.tk]
K.hide
p [(K.token rescue $!.class), K.respond_to?(:token), K.send(:token)]
K.hide_w
p [((K.code = 1) rescue $!.class), K.respond_to?(:code=)]
K.hide_a
K.hide_a2
K.hide_a3
K.hide_t
Q.hide_q
Q.redef
p [(K.seen rescue $!.class), K.respond_to?(:seen), K.send(:seen), K.old]
p [(K.seen2 rescue $!.class), K.respond_to?(:seen2), K.send(:seen2)]
p [(K.seen3 rescue $!.class), K.respond_to?(:seen3), K.send(:seen3)]
p [(K.tail rescue $!.class), K.respond_to?(:tail), K.send(:tail)]
p [(Q.tk rescue $!.class), Q.respond_to?(:tk), Q.send(:tk)]
