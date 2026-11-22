# A define_singleton_method block in a class method that closes over the
# method's locals is refused where a call of a method of that name may run on
# another class: here an unrelated class calls a `mk` of its own, which the
# record cannot tell from a call of this one, so the singleton method might be
# defined on a subclass, with the environment of that run.
class Single
  def self.mk(n)
    define_singleton_method(:g) { n }
    :ok
  end
end
class SubSingle < Single; end
class Other
  def self.mk(n) = n
end
Single.mk(1)
Other.mk(5)
p Single.g
