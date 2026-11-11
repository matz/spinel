# The copy of a literal block in the empty branch runs where the call stood: a method the class defines later is not there yet, and each def binds at its own point.
E = {}
F = {z: 1}
class L
  [1].each_with_object([], **E) { |x, m| p [:empty, m, (u rescue $!.class)] }
  def self.u = :u
end
class N
  [1].each_with_object(**F) { |x, m| p [:nonempty, m, (u rescue $!.class)] }
  def self.u = :u
end
class K2
  def self.v = :early
  [1].each_slice(1, **E) { |s| p [:blk, v] }
  p [:after_split, v]
  def self.v = :late
  [2].each_slice(1, **E) { |s| p [:blk2, v] }
end
def w = :w1
[1].each_slice(1, **E) { |s| p [:top, w] }
def w = :w2
[1].each_slice(1, **E) { |s| p [:top2, w] }
class L2
  [1].each_slice(1, **E) { |s| p [:undef, (u rescue $!.class)] }
  def self.u = :u
end
