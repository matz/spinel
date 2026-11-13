# A visibility word inside an expression may not run when the statement
# holding it does (the right side of `&&` / `||`, a rescue modifier, `||=`,
# an array element, a when's condition past its first). It sets the
# visibility where it runs, and only there: a call with an explicit
# receiver raises for a private top-level def a word that did not run
# would have made public, however the loop reopens Object.

def a1 = 1
c = ARGV.size > 0
c && public(:a1)
def a2 = 1
c || nil || (c && public(:a2))
def a3 = 1
x = (1 rescue public(:a3))
def a4 = 1
y = nil
y ||= 1 || public(:a4)
def a5 = 1
z = [c && public(:a5)]
def a6 = 1
case 1 when 1 then nil when public(:a6) then nil end
def a7 = 1
p((c and public(:a7)))
2.times do
  p((nil.a1 rescue :private))
  p((nil.a2 rescue :private))
  p((nil.a3 rescue :private))
  p((nil.a4 rescue :private))
  p((nil.a5 rescue :private))
  p((nil.a6 rescue :private))
  p((nil.a7 rescue :private))
  p((nil.public_send(:a1) rescue :private))
  class Object
    def a1 = 2
    def a2 = 2
    def a3 = 2
    def a4 = 2
    def a5 = 2
    def a6 = 2
    def a7 = 2
  end
end
p [x, y, z]

# a word that is a statement of its own still sets the visibility where
# it runs
def b1 = 1
public :b1 if ARGV.empty?
2.times do
  p((nil.b1 rescue :private))
  class Object
    def b1 = 2
  end
end

# words inside expressions that run: the def is public from there on
def d1 = 1
t = ARGV.empty?
t && public(:d1)
def d2 = 1
f = ARGV.size > 0
f || public(:d2)
def d3 = 1
w = [public(:d3)].size if t
def d4 = 1
v = (public :d4 if t)
u = [private(:d4)].size if f
def d5 = 1
case 2 when 1 then nil when public(:d5) then nil end
2.times do
  p((nil.d1 rescue :private))
  p((nil.d2 rescue :private))
  p((nil.d3 rescue :private))
  p((nil.d4 rescue :private))
  p((nil.d5 rescue :private))
  p((nil.public_send(:d1) rescue :private))
  class Object
    def d1 = 2
    def d2 = 2
    def d3 = 2
    def d4 = 2
    def d5 = 2
  end
end
p [w, v, u]

# a word naming several methods, by Symbols or one Array, writes each
# name's cell where it runs
def e1 = 1
def e2 = 1
def e3 = 1
def e4 = 1
def e5 = 1
def e6 = 1
t && public(:e1, :e2)
f || public([:e3, :e4])
z = nil
z ||= public(:e5, :e6) if t
def e7 = 1
def e8 = 1
case 2 when 1, public(:e7, :e8) then nil end
def e9 = 1
def e10 = 1
f && public(:e9, :e10)
2.times do
  p [(nil.e1 rescue :private), (nil.e2 rescue :private), (nil.e3 rescue :private), (nil.e4 rescue :private),
     (nil.e5 rescue :private), (nil.e6 rescue :private), (nil.e7 rescue :private), (nil.e8 rescue :private),
     (nil.e9 rescue :private), (nil.e10 rescue :private)]
  class Object
    def e1 = 2
    def e2 = 2
    def e3 = 2
    def e4 = 2
    def e5 = 2
    def e6 = 2
    def e7 = 2
    def e8 = 2
    def e9 = 2
    def e10 = 2
  end
end
p z
