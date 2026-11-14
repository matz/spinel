# A class method made private on its singleton class, then an undef that
# does not run: the class-side queries see the private def, not a public
# one. respond_to? answers false (true with include_all), the call raises,
# and the lists leave the name out. A later public makes it show again.
class J; def self.s = 1; end
class << J; private :s; end
if ARGV.size == 9
  class << J; undef_method :s; end
end

class K; def self.s = 0; end
class L < K; def self.s = 1; end
class << L; private :s; end
if ARGV.size == 9
  class << L; undef_method :s; end
end

class M
  class << self
    private def s = 1
  end
end
class << M; public :s; end
if ARGV.size == 9
  class << M; undef_method :s; end
end

2.times do
  p J.respond_to?(:s)
  p J.respond_to?(:s, true)
  p((J.s rescue $!.class))
  p J.singleton_methods.include?(:s)
  p L.respond_to?(:s)
  p L.respond_to?(:s, true)
  p M.respond_to?(:s)
  p M.singleton_methods.include?(:s)
  p((M.s rescue $!.class))
end
