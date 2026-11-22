# A class-method word in a method body that names a class-side alias of a def
# (`alias` or `alias_method` in `class << self`, or on the singleton class)
# makes the alias private once the body has run, as it does for any class
# method: a call, `respond_to?` and `send` read the version the body's word
# made, and the def the alias copies stays public.
class K
  def self.tok = 6
  class << self
    alias tok3 tok
  end
  def self.hide
    private_class_method :tok3
    nil
  end
end
class L
  def self.tok = 7
  class << self
    alias_method :tok2, :tok
  end
  def self.hide
    private_class_method :tok2
    nil
  end
end
class M
  def self.tok = 8
  singleton_class.alias_method :tok4, :tok
  def self.hide
    private_class_method :tok4
    nil
  end
end
p [K.tok3, L.tok2, M.tok4]
K.hide
L.hide
M.hide
p [(K.tok3 rescue $!.class), K.respond_to?(:tok3), K.send(:tok3), K.tok]
p [(L.tok2 rescue $!.class), L.respond_to?(:tok2), L.send(:tok2), L.tok]
p [(M.tok4 rescue $!.class), M.respond_to?(:tok4), M.send(:tok4), M.tok]
class N
  def self.tok = 9
  class << self
    alias tok5 tok
  end
  def self.hide
    private_class_method :tok5
    nil
  end
end
p N.tok5
N.hide
p [(N.tok5 rescue $!.class), N.respond_to?(:tok5)]
