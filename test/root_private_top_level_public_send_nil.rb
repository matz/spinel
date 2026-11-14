# A public_send on nil of a name whose top-level def is still private where
# the call runs raises the private-method NoMethodError, not the undefined
# one: the call is typed by the def the roots' lookup finds, whatever its
# visibility, and the raise reads the same answer. The sends run once the
# def's `public` word has run too, and a later def of the name on Object
# changes neither.
def m2 = 1
def go(n) = (nil.public_send(n) rescue $!.message)
n1 = ARGV.empty? ? :m2 : :zz
puts go(n1)
puts((nil.public_send(n1) rescue $!.message))
public :m2
2.times do
  puts((nil.m2 rescue $!.message))
  puts((nil.public_send(n1) rescue $!.message))
  class Object
    def m2 = 2
  end
end
