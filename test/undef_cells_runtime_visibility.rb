# A class method with a second version whose visibility only the run tells
# (a def and a `private :s` under a condition): respond_to? reads the
# visibility cell of the live version, so a version that ran with its word
# is private, and one whose branch did not run leaves the first public,
# with an undef that does not run after them. The call itself is left out.
class B; def self.s = 1; end
if ARGV.size == 0
  class B; def self.s = 2; end
  class << B; private :s; end
end

class C; def self.s = 1; end
if ARGV.size == 9
  class C; def self.s = 2; end
  class << C; private :s; end
end
if ARGV.size == 9
  class << C; undef_method :s; end
end

class D; def self.s = 1; end
if ARGV.size == 0
  class D; def self.s = 2; end
  class << D; private :s; end
end
if ARGV.size == 9
  class << D; undef_method :s; end
end

class E; def self.s = 1; end
if ARGV.size == 0
  class << E; private :s; end
end
if ARGV.size == 0
  class << E; public :s; end
end

2.times do
  p [B.respond_to?(:s), B.respond_to?(:s, true)]
  p [C.respond_to?(:s), C.respond_to?(:s, true)]
  p [D.respond_to?(:s), D.respond_to?(:s, true)]
  p [E.respond_to?(:s), E.respond_to?(:s, true)]
end
