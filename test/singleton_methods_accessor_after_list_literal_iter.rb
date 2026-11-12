# A builtin iterator's block on a literal runs at once, though the program
# defines a method of the iterator's name elsewhere (an each of its own
# class's, a top-level times the literal's call does not reach): a list
# there leaves out a class-side accessor made after it, and respond_to?
# there finds no class method defined after it.

class K; def each = yield; end
def times = yield

class C1; end
[1].each { p C1.singleton_methods }
class << C1; attr_accessor :x; end
[1].each { p C1.singleton_methods.sort }

class C2; end
1.times { p C2.singleton_methods }
class << C2; attr_reader :y; end
1.times { p C2.singleton_methods }

class C3; end
[1].each { p C3.respond_to?(:z) }
3.times { p C3.respond_to?(:z) if _1 == 0 }
def C3.z = 1
[1].each { p C3.respond_to?(:z) }

p(K.new.each { :k })
