# A public_send whose name the record cannot read is a call of every name
# it may send, where it stands: one that runs ahead of a visibility word
# meets the version as the def left it, so it raises for a private
# top-level def, and the calls after the word run it.

def m1 = 1
def go(n) = (nil.public_send(n) rescue :private)
n1 = ARGV.empty? ? :m1 : :zz
p go(n1)
public :m1
2.times do
  p((nil.m1 rescue :private))
  p go(n1)
  class Object
    def m1 = 2
  end
end

# the same send at the top level, ahead of the word
def m2 = 1
n2 = ARGV.empty? ? :m2 : :zz
p((nil.public_send(n2) rescue :private))
public :m2
2.times do
  p((nil.m2 rescue :private))
  p((nil.public_send(n2) rescue :private))
  class Object
    def m2 = 2
  end
end

# a send of a run-time name only after the word runs the public version
def m3 = 1
public :m3
n3 = ARGV.empty? ? :m3 : :zz
2.times do
  p((nil.public_send(n3) rescue :private))
  class Object
    def m3 = 2
  end
end
