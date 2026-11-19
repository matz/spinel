# A block given to a program method that may yield at once is kept by the
# record, and a def made later in the same class body is applied as the body
# starts, so a cell would hold it before the block runs: the block meets the
# version live at its point, as CRuby gives it, where it yields at once. A block
# a builtin keeps (a Hash's default block) and runs during the class body meets
# the version at its point too: the defs of its statement are applied as the
# statement starts.
class H
  def self.go = yield
  def self.f = 1
  go { p f }
  go { p to_s }
  def self.f = 2
  def self.to_s = "late"
  T = Hash.new { |_, k| [k, f] }
  p T[0]
end
class H
  def self.f = 3
end
p H.f, H.to_s
class I
  def self.g = yield
  def f = 1
  g { p I.new.f }
  def f = 2
end
p I.new.f
module M
  def self.go = yield
  class << self
    def f = 1
  end
  go { p f }
  class << self
    def f = 2
  end
end
p M.f
