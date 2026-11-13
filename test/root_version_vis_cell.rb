# A visibility word the load record cannot prove for a version (under a
# condition, in a loop, in a block, after a statement that may raise, or
# ahead of a call that may meet the version) sets the visibility the
# version has at run time: a call with an explicit receiver reads it, through
# a visibility cell its face entry's arm tests, and raises for a private (or
# protected) version or runs it.

class Foo; end

# a word that runs, and one that does not
def a1 = 1
public :a1 if ARGV.empty?
def a2 = 1
public :a2 if ARGV.size > 0
2.times do
  p((nil.a1 rescue :private))
  p((nil.a2 rescue :private))
  p((3.a1 rescue :private))
  p((Foo.new.a2 rescue :private))
  p((nil.public_send(:a1) rescue :private))
  class Object
    def a1 = 2
    def a2 = 2
  end
end

# the word inside the loop, and one that flips on each pass
def b1 = 1
3.times do |i|
  public :b1
  p((nil.b1 rescue :private))
  class Object
    def b1 = 2
  end
end
def b2 = 1
3.times do |i|
  p((nil.b2 rescue :private))
  public :b2 if i == 0
  private :b2 if i == 1
  if i == 2
    class Object
      def b2 = 2
    end
  end
end
p((nil.b2 rescue :private))

# a word after the Object def in the loop, which makes it private again
def c1 = 1
2.times do
  p((nil.c1 rescue :private))
  class Object
    def c1 = 2
    private :c1
  end
end
p((nil.c1 rescue :private))

# a call ahead of the word that the arms answer, then after it
def d1 = 1
if ARGV.size > 0
  class Object
    def d1 = 2
  end
end
p((nil.d1 rescue :private))
public :d1
p((nil.d1 rescue :private))
p((nil.public_send(:d1) rescue :private))

# a word in a block run at once, and after a statement that may raise
def e1 = 1
[1].each { public :e1 }
def e2 = 1
begin
  Integer(ARGV.empty? ? "1" : "x")
  public :e2
rescue ArgumentError
end
2.times do
  p((nil.e1 rescue :private))
  p((nil.e2 rescue :private))
  class Object
    def e1 = 2
    def e2 = 2
  end
end

# a protected word that runs: open to main and to an Object's method, closed
# to public_send
def f1 = 1
public :f1
class Object
  protected :f1 if ARGV.empty?
end
class Foo
  def via(o) = (o.f1 rescue :hidden)
end
2.times do
  p((nil.f1 rescue :hidden))
  p Foo.new.via(Foo.new)
  p((nil.public_send(:f1) rescue :hidden))
  class Object
    def f1 = 2
  end
end

# a word whose value is read keeps its value: the cell is written ahead of it
def g1 = 1
x = (public :g1 if ARGV.empty?)
p x
v = [1].map { public :g1 if ARGV.empty? }
p v
2.times do
  p((nil.g1 rescue :private))
  class Object
    private def g1 = 2
  end
end

# a word at exit, which the record cannot place, keeps the cells off: the
# arms keep the defs' visibility, which the word at exit restores
def h1 = 1
at_exit { public :h1; p((nil.h1 rescue :private)) }
2.times do |i|
  p((nil.h1 rescue :private))
  class Object
    def h1 = 2
  end
  private :h1 if i == 1
end
