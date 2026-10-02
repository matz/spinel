# A yielding method of a reopened Object, called without a block on a
# builtin receiver, runs with block_given? false. activesupport's
# Object#try is one. Such a method has no C function of its own. The call
# with a block went through its proc form, but the call without one named
# a function that was never emitted, and the C did not compile.
class Object
  def maybe(*args)
    block_given? ? yield(self, *args) : args.size
  end
end

p 1.maybe(:x)
p "a".maybe
p 2.maybe(3) { |x, y| x * y }
p [1].maybe(4, 5)
p({ k: 1 }.maybe { |h| h.size })
