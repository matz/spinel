# A class-method word in a method body that names a class-side reader or
# alias a later `def self.name` of the class replaced (the def is the method
# the name means when the word runs), or that takes the def itself
# (`private_class_method def self.token = ...`), or that a def in a block or
# under a condition may have replaced (the name's calls read the version cells),
# makes the method private as it does for any class method; so does a word that
# runs, once, before the attribute is made (the attribute then replaces the def).
class K
  class << self
    attr_reader :tok
    alias tok3 tok
  end
  def self.tok = :defd
  def self.tok3 = :redef
  def self.hide
    private_class_method :tok
    private_class_method :tok3
    nil
  end
end
class L
  class << self
    attr_reader :token
  end
  @token = 1
  def self.hide
    private_class_method def self.token = :new
    nil
  end
end
class M
  class << self
    attr_reader :tok
  end
  @tok = 1
  [1].each { def self.tok = :blk }
  def self.hide
    private_class_method :tok
    nil
  end
end
p [K.tok, K.tok3, L.token, M.tok]
K.hide
L.hide
M.hide
p [(K.tok rescue $!.class), K.respond_to?(:tok), K.send(:tok)]
p [(K.tok3 rescue $!.class), K.respond_to?(:tok3), K.send(:tok3)]
p [(L.token rescue $!.class), L.respond_to?(:token), L.send(:token)]
p [(M.tok rescue $!.class), M.respond_to?(:tok), M.send(:tok)]
class N
  def self.hide_n = (private_class_method :tok; nil)
  def self.tok = :defd
end
p [N.tok]
N.hide_n
p [(N.tok rescue $!.class), N.respond_to?(:tok)]
class N
  class << self
    attr_reader :tok
  end
  @tok = :attr
end
p [(N.tok rescue $!.class), N.respond_to?(:tok), N.send(:tok)]
