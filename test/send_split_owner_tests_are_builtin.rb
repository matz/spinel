# The test that splits a send between a class's own send and everyone else's
# is the compiler's: a method of the program's own named like its calls
# (`self.equal?` of a module owner, `is_a?` of an instance owner or of a
# value that is none) does not answer it. A module's own `===` and `<=`
# stay unasked as well.

module M
  def self.send(m, *a) = "M's own send #{m}"
  def self.equal?(o) = false
  def self.hi = "M.hi"
end

module N
  def self.send(m, *a) = "N's own send #{m}"
  def self.===(o) = false
  def self.<=(o) = false
  def self.is_a?(k) = false
  def self.hi = "N.hi"
end

class C
  def self.send(m, *a) = "C's own send #{m}"
  def self.<=(o) = false
  def self.equal?(o) = false
  def self.===(o) = false
  def self.hi = "C.hi"
end

class Sub < C; end

class Q
  def hi = "Q#hi"
  def is_a?(k) = true
end

class R
  def send(m, *a) = "R's own send #{m}"
  def is_a?(k) = false
  def hi = "R#hi"
end

[M, N, C, Sub, Q.new, R.new, 3].each do |v|
  puts((v.send(:hi) rescue "#{$!.class}"))
end
