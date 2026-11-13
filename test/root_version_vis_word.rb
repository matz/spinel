# A visibility word (`public :m`) after a top-level def changes the
# visibility of that def's version. A call with an explicit receiver binds
# the face of the name's version entry, whose arm for each version raises by
# the visibility the load record proves the version has: its def's, or a
# word's that surely runs on it ahead of every call that may meet it. A word
# that may not run, runs on another version or comes after the version's
# calls leaves the def's, so the private arm still raises.

class Foo; end

# `public :m` ahead of a loop whose body reopens Object
def a1 = 1
public :a1
2.times do
  p((nil.a1 rescue :private))
  class Object
    def a1 = 2
  end
end

# on an Integer, a program object, through public_send, with an argument
def a2(x) = x
public :a2
2.times do
  p((3.a2(5) rescue :private))
  p((Foo.new.a2(6) rescue :private))
  p((nil.public_send(:a2, 7) rescue :private))
  class Object
    def a2(x) = x * 2
  end
end

# the word in Object's own body, a protected one, and a flip that ends public
def a3 = 1
class Object
  public :a3
end
def a4 = 1
class Object
  protected :a4
end
def a5 = 1
public :a5
private :a5
public :a5
2.times do
  p((nil.a3 rescue :private))
  p((nil.a4 rescue :hidden))
  p((nil.a5 rescue :private))
  class Object
    def a3 = 2
    def a4 = 2
    def a5 = 2
  end
end

# a word that keeps its version's visibility leaves the private arm raising
def a7 = 1
2.times do
  p((nil.a7 rescue :private))
  class Object
    def a7 = 2
  end
  public :a7
end

# a word on a version that a later def replaces, and one that does not run
def a8 = 1
public :a8
def a8 = 3
2.times do
  p((nil.a8 rescue :private))
  class Object
    def a8 = 2
  end
  def a8 = 4
end
def a9 = 1
public :a9 if ARGV.size > 0
2.times do
  p((nil.a9 rescue :private))
  class Object
    def a9 = 2
  end
end

# a call in a method defined after the word, and a word on one name only
def b1 = 1
def b2 = 1
public :b1
def via(x) = (x.b1 rescue :private)
2.times do
  p via(nil)
  p((nil.b2 rescue :private))
  class Object
    def b1 = 2
    def b2 = 2
  end
end

# a call ahead of the word, where the top-level def is the live version
def b3 = 1
p((nil.b3 rescue :private))
public :b3
2.times do
  p((nil.b3 rescue :private))
  class Object
    def b3 = 2
  end
end

# a public_send of a name known only at run time, after the word: the
# word's visibility on the top-level def's arm, the private Object def's
# arm raises
def b4 = 1
public :b4
def dgo4(n) = (nil.public_send(n) rescue :private)
n4 = [:b4].first
2.times do
  p dgo4(:b4)
  p((2.public_send(n4) rescue :private))
  class Object
    private def b4 = 2
  end
end

# a word in a body that runs later than its statement (at exit, in a
# Proc) is one the record cannot place: the arms keep the defs'
# visibility, not the earlier word's
def h1 = 1
public :h1
at_exit { private :h1; p((nil.h1 rescue :private)) }
class Object; def h1 = 2; end if ARGV.size > 0
def h2 = 1
public :h2
pr = proc { private :h2 }
class Object; def h2 = 2; end if ARGV.size > 0
pr.call
p((nil.h2 rescue :private))
