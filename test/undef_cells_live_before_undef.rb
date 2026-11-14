# A class method's def ahead of an undef that does not run stays live: the
# class-side queries see the def, not the undef's answer.
class K; def self.s = 1; end
class J < K; def self.s = 2; end
if ARGV.size == 9
  class << J; undef_method :s; end
end

class L; def self.s = 2; end
if ARGV.size == 9
  class << L; undef_method :s; end
end

2.times do
  p J.respond_to?(:s)
  p J.singleton_methods.include?(:s)
  p((J.s rescue $!.class))
end
2.times do
  p L.respond_to?(:s)
  p L.singleton_methods.include?(:s)
  p((L.s rescue $!.class))
end
